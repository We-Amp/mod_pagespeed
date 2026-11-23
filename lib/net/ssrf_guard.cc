// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// inet_aton (BSD) is gated behind _DEFAULT_SOURCE on glibc under a strict
// -std=c++NN; define it BEFORE any system header so the declaration is visible on
// Linux (it is unconditionally available on macOS/BSD). Must precede all includes.
#ifndef _DEFAULT_SOURCE
// NOLINTNEXTLINE(bugprone-reserved-identifier) — standard glibc feature macro.
#define _DEFAULT_SOURCE 1  // NOLINT(bugprone-reserved-identifier)
#endif

#include "lib/net/ssrf_guard.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>  // inet_pton, inet_aton, inet_ntop, AF_INET, AF_INET6
#endif

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Port of agent-readability-scanner/src/ssrf.mjs. Every range rule below cites
// the corresponding ssrf.mjs line so the two stay decision-identical. The intent
// is byte-for-byte parity of the allow/deny verdict on any given address; where
// the C++ deviates it deviates in the SAFE (more-blocking) direction only, and
// such cases are commented.

namespace pagespeed {
namespace {

// Parse a legacy / obfuscated IPv4 literal (hex / octal / dword forms) into an
// in_addr. POSIX uses inet_aton; Windows lacks it, so inet_addr — the lenient
// equivalent that accepts the same forms — stands in. inet_addr returns
// INADDR_NONE both on error and for 255.255.255.255, so that one address is
// accepted explicitly to preserve inet_aton's behavior.
int ParseLegacyIpv4(const char* cp, struct in_addr* inp) {
#ifdef _WIN32
  unsigned long a = inet_addr(cp);
  if (a == INADDR_NONE && std::strcmp(cp, "255.255.255.255") != 0) return 0;
  inp->s_addr = a;
  return 1;
#else
  return inet_aton(cp, inp);
#endif
}

std::string ToLowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

int SchemeDefaultPort(std::string_view scheme) {
  if (scheme == "http") return 80;
  if (scheme == "https") return 443;
  return 0;  // unknown scheme — caller (IsAllowedScheme) rejects it anyway
}

// Parse one IPv4 dotted label with JS Number() coercion for the forms that occur
// in practice (decimal incl. leading-zero-as-decimal, 0x-hex, ""==0). Returns
// nullopt for anything else (scientific notation, signs, garbage) — treated by
// the caller as out-of-range => BLOCK (fail-closed; stricter-than-JS only for
// inputs net.isIP would never have produced as a canonical IPv4 anyway).
// Uses a FIXED-WIDTH uint64_t accumulator (NOT `long`): `long` is 32-bit on
// win-x64 (LLP64), where v*16/v*10 past 2^31 is signed-overflow UB that wraps and
// can flip a verdict (e.g. 0x100000001 -> 1 -> a public-looking octet). The
// post-add early-return at >0x7fffffff stops well before uint64 can overflow.
std::optional<uint64_t> JsNumberLabel(std::string_view s) {
  if (s.empty()) return uint64_t{0};  // JS: Number("") === 0
  // Hex: 0x / 0X prefix (JS: Number("0x7f") === 127).
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    uint64_t v = 0;
    for (size_t i = 2; i < s.size(); ++i) {
      const char c = s[i];
      int d;
      if (c >= '0' && c <= '9') {
        d = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        d = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        d = c - 'A' + 10;
      } else {
        return std::nullopt;
      }
      v = v * 16 + static_cast<uint64_t>(d);
      if (v > 0x7fffffffu) return v;  // exceeds any octet; stop before overflow
    }
    return v;
  }
  // Decimal (leading zeros are decimal in JS Number(), NOT octal).
  uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<uint64_t>(c - '0');
    if (v > 0x7fffffffu) return v;
  }
  return v;
}

// ssrf.mjs lines :44-58 — the embedded trailing dotted-quad check inside V6.
// Returns the trailing "a.b.c.d" if `s` ends with one, else nullopt. Mirrors the
// JS regex /(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})$/ (exactly 4 groups, 1-3 digits).
std::optional<std::string> TrailingDottedQuad(std::string_view s) {
  // Walk back over the pattern d{1,3}.d{1,3}.d{1,3}.d{1,3} anchored at end.
  size_t i = s.size();
  auto take_dec = [&](size_t& pos) -> int {  // consume 1..3 trailing digits
    int n = 0;
    while (pos > 0 && n < 3 && s[pos - 1] >= '0' && s[pos - 1] <= '9') {
      --pos;
      ++n;
    }
    return n;
  };
  auto take_dot = [&](size_t& pos) -> bool {
    if (pos > 0 && s[pos - 1] == '.') {
      --pos;
      return true;
    }
    return false;
  };
  if (take_dec(i) == 0) return std::nullopt;
  if (!take_dot(i)) return std::nullopt;
  if (take_dec(i) == 0) return std::nullopt;
  if (!take_dot(i)) return std::nullopt;
  if (take_dec(i) == 0) return std::nullopt;
  if (!take_dot(i)) return std::nullopt;
  if (take_dec(i) == 0) return std::nullopt;
  return std::string(s.substr(i));
}

// Strict canonical dotted-decimal IPv4 test matching Node's net.isIPv4 (the gate
// ssrf.mjs's isPrivateIp relies on): exactly 4 groups, each 1-3 digits, value
// 0-255, NO leading zeros. This deliberately does NOT use the platform inet_pton:
// macOS/BSD inet_pton leniently accepts leading-zero forms like "0177.0.0.1"
// (which a downstream resolver may treat as octal 127.0.0.1), which would be an
// SSRF bypass — Node's net.isIP rejects them, so we must too (=> treated as a
// hostname and caught at the resolved-IP stage, or denied as unparseable).
bool IsCanonicalV4(std::string_view s) {
  int groups = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '.') {
      std::string_view g = s.substr(start, i - start);
      if (g.empty() || g.size() > 3) return false;
      for (const char c : g) {
        if (c < '0' || c > '9') return false;
      }
      if (g.size() > 1 && g[0] == '0') return false;  // no leading zeros
      int val = 0;
      for (const char c : g) val = val * 10 + (c - '0');
      if (val > 255) return false;
      if (++groups > 4) return false;
      start = i + 1;
    }
  }
  return groups == 4;
}

struct LiteralClass {
  bool is_literal = false;
  std::string canonical;
};

// Classify `host` (lower-cased, IPv6 brackets already stripped) as an IP literal
// in ANY form a WHATWG special-scheme URL parser accepts, returning its canonical
// textual form. This mirrors `new URL().hostname`'s IPv4 normalization so encoded
// loopback forms (hex/octal/dword, e.g. 0x7f000001 / 2130706433 / 0177.0.0.1)
// cannot slip past the literal branch into the caller-trusted resolved-address
// branch. ssrf.mjs gets this for free via new URL(); we must do it explicitly.
LiteralClass ClassifyHostLiteral(std::string_view host) {
  // IPv4: WHATWG strips a single trailing root dot for the v4 parse. Accept the
  // strict-canonical form, else legacy hex/octal/dword forms via inet_aton, and
  // canonicalize to dotted-decimal so the verdict is computed on the real address
  // (NOT a blanket block: 01.01.01.01 -> 1.1.1.1 is public, ssrf.mjs allows it).
  std::string h4(host);
  if (!h4.empty() && h4.back() == '.') h4.pop_back();
  if (IsCanonicalV4(h4)) return {true, h4};
  if (!h4.empty()) {
    in_addr a4{};
    if (ParseLegacyIpv4(h4.c_str(), &a4) != 0) {
      char buf[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &a4, buf, sizeof(buf)) != nullptr) {
        return {true, std::string(buf)};
      }
    }
  }
  // IPv6: classify on the zone-stripped form so a %zone literal is recognized AS a
  // literal (validated directly), not deferred to the caller's resolved addresses.
  std::string h6(host);
  const size_t pct = h6.find('%');
  if (pct != std::string::npos) h6.resize(pct);
  if (!h6.empty()) {
    unsigned char b6[16];
    if (inet_pton(AF_INET6, h6.c_str(), b6) == 1) return {true, h6};
  }
  return {false, std::string(host)};
}

// The IPv4 addresses an IPv6 address can carry, so a v4 rule (private,
// loopback, link-local) is applied to each: v4-mapped ::ffff:0:0/96,
// v4-compatible ::/96, NAT64 64:ff9b::/96 (RFC 6052), the local-use NAT64
// range 64:ff9b:1::/48 (RFC 8215; both the /48 and the /96 embedding
// positions, since the operator picks the prefix length), and 6to4
// 2002::/16 (RFC 3056).  `b` is the address in network order.
std::vector<std::string> EmbeddedV4Candidates(const unsigned char b[16]) {
  std::vector<std::string> out;
  auto dotted = [&out](unsigned char a, unsigned char c, unsigned char d,
                       unsigned char e) {
    out.push_back(std::to_string(a) + "." + std::to_string(c) + "." +
                  std::to_string(d) + "." + std::to_string(e));
  };
  auto zero = [b](int from, int to) {
    for (int i = from; i < to; ++i) {
      if (b[i] != 0) return false;
    }
    return true;
  };
  if (zero(0, 10) && b[10] == 0xff && b[11] == 0xff) {
    dotted(b[12], b[13], b[14], b[15]);  // v4-mapped
  } else if (zero(0, 12)) {
    dotted(b[12], b[13], b[14], b[15]);  // v4-compatible
  } else if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
    if (zero(4, 12)) {
      dotted(b[12], b[13], b[14], b[15]);  // 64:ff9b::/96
    } else if (b[4] == 0x00 && b[5] == 0x01) {
      dotted(b[6], b[7], b[9], b[10]);     // 64:ff9b:1::/48
      dotted(b[12], b[13], b[14], b[15]);  // a /96 inside it
    }
  } else if (b[0] == 0x20 && b[1] == 0x02) {
    dotted(b[2], b[3], b[4], b[5]);  // 6to4
  } else if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00) {
    // Teredo 2001::/32 (RFC 4380): the server address in bits 32-63, the
    // client address obfuscated (inverted) in the last 32 bits.
    dotted(b[4], b[5], b[6], b[7]);
    dotted(
        static_cast<unsigned char>(~b[12]), static_cast<unsigned char>(~b[13]),
        static_cast<unsigned char>(~b[14]), static_cast<unsigned char>(~b[15]));
  }
  // SIIT IPv4-translated ::ffff:0:a.b.c.d (RFC 2765).
  if (zero(0, 8) && b[8] == 0xff && b[9] == 0xff && b[10] == 0 && b[11] == 0) {
    dotted(b[12], b[13], b[14], b[15]);
  }
  // ISATAP interface identifier ::0:5efe:a.b.c.d / ::200:5efe:a.b.c.d
  // (RFC 5214), under any prefix.
  if ((b[8] == 0x00 || b[8] == 0x02) && b[9] == 0x00 && b[10] == 0x5e &&
      b[11] == 0xfe) {
    dotted(b[12], b[13], b[14], b[15]);
  }
  return out;
}

// Parse a textual IPv6 address (zone id dropped) to bytes.
bool ParseV6Bytes(std::string_view ip, unsigned char out[16]) {
  std::string z(ip);
  const size_t pct = z.find('%');
  if (pct != std::string::npos) z.resize(pct);
  return !z.empty() && inet_pton(AF_INET6, z.c_str(), out) == 1;
}

}  // namespace

bool IsPrivateV4(std::string_view ip) {
  // ssrf.mjs:24-26 — split on '.', Number() each, require exactly 4 in [0,255].
  std::array<uint64_t, 4> p{};
  int count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= ip.size(); ++i) {
    if (i == ip.size() || ip[i] == '.') {
      if (count >= 4) return true;  // too many labels
      std::optional<uint64_t> v = JsNumberLabel(ip.substr(start, i - start));
      if (!v.has_value() || *v > 255) return true;  // fail-closed (unsigned)
      p[count++] = *v;
      start = i + 1;
    }
  }
  if (count != 4) return true;  // ssrf.mjs: p.length !== 4 => block
  const uint64_t a = p[0], b = p[1], c = p[2];
  if (a == 0) return true;                           // 0.0.0.0/8            :28
  if (a == 10) return true;                          // RFC1918 10/8        :29
  if (a == 127) return true;                         // loopback 127/8      :30
  if (a == 169 && b == 254) return true;             // link-local+metadata :31
  if (a == 172 && b >= 16 && b <= 31) return true;   // RFC1918 172.16/12 :32
  if (a == 192 && b == 168) return true;             // RFC1918 192.168/16  :33
  if (a == 192 && b == 0 && c == 0) return true;     // 192.0.0.0/24      :34
  if (a == 100 && b >= 64 && b <= 127) return true;  // CGNAT 100.64/10  :35
  if (a == 198 && (b == 18 || b == 19)) return true;  // benchmark 198.18/15 :36
  if (a >= 224) return true;                          // multicast/reserved  :37
  return false;
}

bool IsPrivateV6(std::string_view ip) {
  // ssrf.mjs:42 — lower-case, drop the %zone-id suffix.
  std::string s = ToLowerAscii(ip);
  const size_t pct = s.find('%');
  if (pct != std::string::npos) s.resize(pct);

  if (s == "::1" || s == "::") return true;  // :43
  if (std::optional<std::string> q = TrailingDottedQuad(s);
      q.has_value() && IsPrivateV4(*q)) {
    return true;  // :45-46
  }
  // :47 — ALL `::ffff:`-prefixed are blocked (v4-mapped "without dotted form →
  // unsafe"); note this fires even for a PUBLIC embedded v4, exactly as ssrf.mjs
  // does (the startsWith check is unconditional after the embedded-private test).
  if (s.rfind("::ffff:", 0) == 0) return true;
  if (s.size() >= 2 && s[0] == 'f' && (s[1] == 'c' || s[1] == 'd')) {
    return true;  // ULA  :48
  }
  if (s.size() >= 3 && s[0] == 'f' && s[1] == 'e' &&
      (s[2] == '8' || s[2] == '9' || s[2] == 'a' || s[2] == 'b')) {
    return true;  // ll   :49
  }
  if (s.rfind("ff", 0) == 0) return true;        // mcast :50
  if (s.rfind("2001:db8", 0) == 0) return true;  // doc  :51
  // Beyond ssrf.mjs: the binary forms the textual rules above miss.  Every
  // v4-mapped address in any spelling is blocked (as the ::ffff: text rule
  // intends), the local-use NAT64 range is never a public address, and an
  // IPv4 address embedded by v4-compatible, NAT64 or 6to4 form is judged as
  // that IPv4 address.
  unsigned char b[16];
  if (ParseV6Bytes(s, b)) {
    bool mapped = true;
    for (int i = 0; i < 10; ++i) mapped = mapped && b[i] == 0;
    if (mapped && b[10] == 0xff && b[11] == 0xff) return true;
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b &&
        b[4] == 0x00 && b[5] == 0x01) {
      return true;  // 64:ff9b:1::/48
    }
    // Teredo 2001::/32: a tunnel endpoint, never a backend address.
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00) {
      return true;
    }
    // Deprecated site-local fec0::/10 (RFC 3879).
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return true;
    // Discard-only 100::/64 (RFC 6666).
    bool discard = b[0] == 0x01 && b[1] == 0x00;
    for (int i = 2; i < 8; ++i) discard = discard && b[i] == 0;
    if (discard) return true;
    for (const std::string& v4 : EmbeddedV4Candidates(b)) {
      if (IsPrivateV4(v4)) return true;
    }
  }
  return false;
}

bool IsPrivateIp(std::string_view ip) {
  // A NUL byte never appears in real resolver/inet_ntop output; reject it up front
  // so a `::1\0x` literal can't classify as the valid prefix `::1` under c_str()
  // truncation while the textual rules see the full (non-matching) string.
  if (ip.find('\0') != std::string_view::npos) return true;
  // ssrf.mjs:55-60 — dispatch on net.isIP; not a parseable literal => block.
  // v4 uses our STRICT canonical check (not the lenient platform inet_pton) so
  // leading-zero/octal bypass forms are denied identically on every platform.
  if (IsCanonicalV4(ip)) return IsPrivateV4(ip);
  const std::string z(ip);
  unsigned char buf[16];
  if (inet_pton(AF_INET6, z.c_str(), buf) == 1) {
    // Run the TEXTUAL v6 rules on the original string (e.g. a non-dotted
    // `::ffff:7f00:1` is denied as text even though inet_pton accepts it).
    return IsPrivateV6(ip);
  }
  return true;  // unparseable / non-canonical => deny (fail-closed)
}

bool IsLinkLocalIp(std::string_view ip) {
  const std::string z(ip);
  unsigned char buf[16];
  if (inet_pton(AF_INET, z.c_str(), buf) == 1) {
    return buf[0] == 169 &&
           buf[1] == 254;  // 169.254.0.0/16 (incl. metadata .254)
  }
  if (ParseV6Bytes(z, buf)) {
    if (buf[0] == 0xfe && (buf[1] & 0xc0) == 0x80) return true;  // fe80::/10
    // An IPv4 link-local address carried inside an IPv6 one (v4-mapped,
    // v4-compatible, NAT64, 6to4) reaches the same endpoint.
    for (const std::string& v4 : EmbeddedV4Candidates(buf)) {
      if (IsLinkLocalIp(v4)) return true;
    }
    return false;
  }
  return false;  // not an IP literal => not link-local
}

bool IsAllowedScheme(std::string_view scheme) {
  std::string s = ToLowerAscii(scheme);
  if (!s.empty() && s.back() == ':') s.pop_back();  // tolerate "http:" form
  return s == "http" || s == "https";
}

bool IsBlockedHostLiteral(std::string_view host) {
  std::string h = ToLowerAscii(host);
  // strip IPv6 brackets and a single trailing dot (FQDN root)
  if (h.size() >= 2 && h.front() == '[' && h.back() == ']') {
    h = h.substr(1, h.size() - 2);
  }
  if (!h.empty() && h.back() == '.') h.pop_back();
  // Cloud metadata special names (spec §3.3.4). The metadata IPs themselves
  // (169.254.169.254, fd00:ec2::254) are already covered by IsPrivateV4/V6.
  return h == "metadata" || h == "metadata.google.internal";
}

std::optional<UrlAuthority> ParseHttpUrl(std::string_view raw_url) {
  // WHATWG normalization BEFORE structural parse, so authority-terminator and
  // userinfo handling match `new URL()` (the reference ssrf.mjs uses):
  //   (1) tab/LF/CR are removed from the URL entirely; and
  //   (2) for special schemes (http/https) a backslash is a forward slash.
  // Without these, `http://127.0.0.1\@evil.com/` and `http://127.0.0.1<TAB>/`
  // would pick the wrong (public-looking) host while a browser connects to
  // loopback — the forbidden more-permissive-than-reference direction.
  std::string url;
  url.reserve(raw_url.size());
  for (const char c : raw_url) {
    if (c == '\t' || c == '\n' || c == '\r') continue;  // stripped (WHATWG)
    url.push_back(c == '\\' ? '/' : c);  // special-scheme backslash
  }

  // Require a "scheme://" form (new URL() throws otherwise for http(s)).
  const std::string_view uv(url);
  const size_t sep = uv.find("://");
  if (sep == std::string_view::npos || sep == 0) return std::nullopt;
  UrlAuthority out;
  out.scheme = ToLowerAscii(uv.substr(0, sep));

  std::string_view rest = uv.substr(sep + 3);
  // Authority ends at the first '/', '?', or '#'.
  const size_t end = rest.find_first_of("/?#");
  std::string_view authority =
      (end == std::string_view::npos) ? rest : rest.substr(0, end);
  if (authority.empty()) return std::nullopt;

  // Userinfo: host is whatever follows the LAST '@' (WHATWG semantics).
  const size_t at = authority.rfind('@');
  out.had_userinfo = (at != std::string_view::npos);
  std::string_view hostport =
      out.had_userinfo ? authority.substr(at + 1) : authority;
  if (hostport.empty()) return std::nullopt;

  std::string host;
  std::string_view port_sv;
  bool have_port = false;
  if (hostport.front() == '[') {
    // IPv6 literal: keep only the bracket contents; a port may follow ']'.
    const size_t close = hostport.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    host = std::string(hostport.substr(1, close - 1));
    std::string_view after = hostport.substr(close + 1);
    if (!after.empty()) {
      if (after.front() != ':') return std::nullopt;  // junk after ]
      port_sv = after.substr(1);
      have_port = true;
    }
  } else {
    // host is up to the port separator ':'.
    const size_t colon = hostport.find(':');
    if (colon == std::string_view::npos) {
      host = std::string(hostport);
    } else {
      host = std::string(hostport.substr(0, colon));
      port_sv = hostport.substr(colon + 1);
      have_port = true;
    }
  }
  if (host.empty()) return std::nullopt;
  out.host = ToLowerAscii(host);

  // Port: explicit decimal in [0,65535], else the scheme default. A malformed or
  // out-of-range port makes new URL() throw, so we reject (fail-closed).
  if (have_port && !port_sv.empty()) {
    uint32_t p = 0;
    for (const char c : port_sv) {
      if (c < '0' || c > '9') return std::nullopt;
      p = p * 10 + static_cast<uint32_t>(c - '0');
      if (p > 65535) return std::nullopt;
    }
    out.port = static_cast<int>(p);
  } else {
    out.port = SchemeDefaultPort(out.scheme);  // absent or "host:" → default
  }

  // Reject any host with a control char, space, NUL, or '%' that survived
  // (fail-closed). Control/space/NUL must never reach a resolver or split a host
  // string. A '%' carries an IPv6 zone-id (fe80::1%eth0): the zone is load-bearing
  // for link-local routing, and stripping-then-comparing it would let one zone
  // over-match the upstream pin and skip the G2 guard — so a zoned (or '%'-bearing)
  // host is simply not a valid pin target here. Link-local upstreams are unsupported.
  for (const unsigned char c : out.host) {
    if (c <= 0x20 || c == '%') return std::nullopt;
  }

  // Classify the host as an IP literal in ANY form a special-scheme URL parser
  // accepts (canonical/legacy IPv4, IPv6 incl. %zone), canonicalizing so encoded
  // loopback forms can't slip into the caller-trusted resolved-address branch.
  const LiteralClass lit = ClassifyHostLiteral(out.host);
  out.host_is_ip_literal = lit.is_literal;
  if (lit.is_literal) out.host = lit.canonical;
  return out;
}

std::optional<ResolveSafeResult> ResolveSafe(
    std::string_view raw_url,
    const std::vector<std::string>& resolved_addresses) {
  std::optional<UrlAuthority> u = ParseHttpUrl(raw_url);
  if (!u.has_value()) return std::nullopt;               // invalid URL
  if (!IsAllowedScheme(u->scheme)) return std::nullopt;  // scheme not allowed
  if (IsBlockedHostLiteral(u->host))
    return std::nullopt;  // metadata special name

  ResolveSafeResult result;
  result.host = u->host;

  if (u->host_is_ip_literal) {
    // ssrf.mjs: `if (net.isIP(host)) { if (isPrivateIp(host)) throw; return [host]; }`
    if (IsPrivateIp(u->host)) return std::nullopt;
    result.safe_addresses.push_back(u->host);
    return result;
  }

  // Hostname: the caller must have resolved it; every address must be public.
  if (resolved_addresses.empty()) return std::nullopt;  // no addresses => block
  for (const std::string& addr : resolved_addresses) {
    if (IsPrivateIp(addr)) return std::nullopt;  // any private => block
  }
  result.safe_addresses = resolved_addresses;
  return result;
}

bool IsPublicUrl(std::string_view raw_url,
                 const std::vector<std::string>& resolved_addresses) {
  return ResolveSafe(raw_url, resolved_addresses).has_value();
}

}  // namespace pagespeed
