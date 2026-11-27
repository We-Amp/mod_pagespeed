/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// A stand-in for the optimizer daemon's client library, as a real shared
// object that can actually be dlopen()ed.
//
// This exists because LoadDaemonAbi is the one part of the adapter that no
// injected fake can reach: every other test replaces the loader wholesale, so
// the symbol binding, the version gate and the layout handshake had never
// executed anywhere. A stub .so is the only way to run them.
//
// Behaviour is steered by environment variables so one artifact covers every
// case; each is read fresh on every call, so a test can change its mind
// between loads.
//
//   PS_STUB_MAJOR / PS_STUB_MINOR  reported API version (default 1 / 12)
//   PS_STUB_STRUCT_SIZE            what the UNSIZED initializer stamps into
//                                  struct_size (default: the real size)
//   PS_STUB_SCRIBBLE               bytes the SIZED initializer writes past
//                                  the size it was given (default 0)
//   PS_STUB_VOLUME_SIZE            what the shared-config reader returns
//   PS_STUB_GENERATION             what the cache-dir-generation reader
//                                  returns (default 3, the current generation)
//   PS_STUB_LOG                    file the record arm's calls are appended
//                                  to, one `key=value ...` line each, so a
//                                  test can assert what the peer was ASKED
//                                  rather than what the caller believes it
//                                  asked
//   PS_STUB_NOTIFY_FAIL            non-zero: every notification fails with an
//                                  I/O error, without a socket
//   PS_STUB_BODY_MAGIC             hex bytes written over the start of every
//                                  entry's body, so a case can store an entry
//                                  whose CONTENT is a real image signature
//                                  independently of the id it sits at
//   PS_STUB_CLASSIFY_VIEWPORT      the viewport field ps_classify stamps,
//                                  overriding the User-Agent derivation -- a
//                                  DEFECT SHAPE the real classifier cannot
//                                  produce (the reserved value 3 is the
//                                  sentinel marker), steerable so the serve
//                                  arm's sentinel guard is testable against
//                                  the one input it exists for
//   PS_STUB_READ_ALT_STATUS        non-zero: every by-id read fails with this
//                                  code (one of the peer's I/O, corrupted,
//                                  busy or closed values) before any
//                                  alternate is consulted, so a caller is
//                                  testable against a probe that answers
//                                  neither found nor not-found
//   PS_STUB_ORIGINAL_FLAGS_SEQUENCE
//                                comma-separated hex flags bytes, e.g.
//                                "00,08": the n-th by-id read of the durable
//                                original (id 0x0C, ps_cache_read_alternate)
//                                reports the n-th value as its flags byte
//                                instead of the table's, and the last value
//                                repeats.  Models an original re-recorded
//                                between the arm's two reads of it.  The
//                                count restarts whenever the string changes
//                                (unset it between cases that reuse a spec).
//   PS_STUB_SITE_LOG               file each sited cache call is appended
//                                  to as "<op> site=<site> url=<url>"
//   PS_STUB_SITE                   when set, the keyspace the stored entries
//                                  belong to: a sited read under any other
//                                  site (or none) finds nothing, as the real
//                                  library's per-site keys do
//
// THE CACHE ENTRY POINTS ARE THE SITED ONES ONLY.  ps_cache_write_original,
// ps_cache_read_best and ps_cache_read_alternate are not exported at all, so
// a module that resolved any of them would fail to load this library: every
// suite that loads it proves the module binds the _site entry points alone.
//
// Two cases cannot be environment variables, because a symbol cannot be
// un-exported at run time and its PRESENCE is what the loader acts on. Each is
// a separate build of this file, and a separate target; see the BUILD file.
//
//   PS_STUB_OMIT_SIZED_INIT      no ps_cache_config_init_sized: an older
//                                daemon the loader must still accept, because
//                                that entry point is bound OPTIONALLY.
//   PS_STUB_OMIT_VARIES_ACCEPT   no ps_vary_varies_accept, while still
//                                REPORTING a version at or above the floor.
//                                Not a version this module can meet halfway:
//                                the symbol is bound unconditionally, so the
//                                library is refused. This flavour is what
//                                tells the two bindings apart -- against a
//                                truthful library an optional binding and a
//                                required one behave identically, and only a
//                                library that lies about its version shows
//                                which one is in use.
//   PS_STUB_OMIT_GENERATION      no ps_read_shared_config_generation: the
//                                pre-H1 daemon. Bound OPTIONALLY, so this
//                                library must still load -- the adapter
//                                tolerates the absence as the legacy layout.
//                                It also lacks the later, equally optional
//                                ps_serve_stats_record_hit_host.
//   PS_STUB_OMIT_LAST_ERROR      no ps_last_error_message: a daemon package
//                                from before the per-failure explanation was
//                                published. Bound OPTIONALLY, so it must
//                                still load; the degrade is that an error
//                                line carries the error class alone.
//   PS_STUB_OMIT_SITE            no _site cache entry points, while still
//                                REPORTING 1.12: refused, because they are
//                                bound unconditionally.
//   PS_STUB_HTML_SERVE           exports the HTML serve set (1.13 + 1.14).
//                                Bound only when the reported minor is 14 or
//                                later, so a case sets PS_STUB_MINOR.  Its
//                                answers are steered by PS_STUB_HTML_SERVE_*,
//                                PS_STUB_HTML_MODE, PS_STUB_AGENT_ENTITLED,
//                                PS_STUB_SIDECAR_WRITE_STATUS,
//                                PS_STUB_REMOVE_STATUS and
//                                PS_STUB_EPOCH_{WRITE,READ}_STATUS.
//   PS_STUB_OMIT_SITE_PURGE_EPOCH_READ
//                                with PS_STUB_HTML_SERVE: the set without
//                                ps_cache_read_site_purge_epoch.  The set is
//                                bound as a whole, so none of it is used.
//   PS_STUB_HTML_SERVE_15        with PS_STUB_HTML_SERVE: also the 1.15 set
//                                (request guard, cookie classification, the
//                                ignore-list guard, policy stamp, the
//                                unsafe-method predicate, the block's stamp
//                                and the facts call that reads it), and the
//                                1.15 serve verdicts: a serve-cached-html
//                                request short of the 1.15 size falls
//                                through with `scope`.  Bound only when the
//                                reported minor is 15 or later.
//   PS_STUB_OMIT_UNSAFE_METHOD_INVALIDATES
//                                with PS_STUB_HTML_SERVE_15: the 1.15 set
//                                without ps_html_unsafe_method_invalidates,
//                                the LAST one bound, so none of it is used.
//   PS_STUB_HTML_SERVE_16        with PS_STUB_HTML_SERVE_15: also the 1.16
//                                set (the coding choice, the coded facts,
//                                the coded tag and the published codings,
//                                PS_STUB_MARKDOWN_CODINGS, default 3), the
//                                reason no_acceptable_encoding, and the 1.16
//                                verdict field: a verdict of the 1.16 size
//                                to serve Markdown names the coding asked
//                                for when its BOUND fact is set, else
//                                identity with IDENTITY_OK, else falls
//                                through.  Bound only from minor 16.
//   PS_STUB_OMIT_MARKDOWN_SELECT_ENCODING
//                                with PS_STUB_HTML_SERVE_16: the 1.16 set
//                                without ps_markdown_select_encoding, the
//                                LAST one bound, so none of it is used.
//   PS_STUB_ECHO_SIZES (setting) every serve verdict carries the request's
//                                struct_size in age_s and the facts'
//                                struct_size in not_modified, so a test sees
//                                the sizes the binding sent.
//
// WHY THIS FILE MODELS THE PEER'S VALIDATION RATHER THAN JUST SUCCEEDING.
// A stand-in that accepts everything cannot fail the way the real library
// fails, so the tests over it pass whatever the caller does -- which is how a
// pair ships non-functional with every test green. Every rule below was read
// off the published contract and is reproduced with its exact fail direction:
// the alternate-id and mask-low-byte gates, the ladder that snaps a declared
// struct size DOWN to the nearest published one (so a caller whose struct is
// the wrong size silently loses the origin state rather than being told), the
// content cap in both of its enforcement positions, the both-or-neither
// option-context rule with the signature RECOMPUTED rather than trusted, and
// the notification's struct-size thresholds.

// The flavour without the sited cache calls leaves their helpers unused.
#if defined(PS_STUB_OMIT_SITE) && defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-const-variable"
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
// WIN32_LEAN_AND_MEAN and NOMINMAX keep the header's COM and min/max macros
// out of this file (they collide with it, measured); they must be set before
// the header is first pulled in.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

// Layout-identical to the module's PsCacheConfig and to the daemon's real
// ps_cache_config_t. Duplicated rather than included: this file is meant to
// behave like an independently built peer.
struct StubCacheConfig {
  size_t struct_size;
  const char* volume_path;
  uint64_t volume_size;
  int enable_checksum;
  size_t ram_cache_size;
  size_t max_metadata_size;
};

#ifdef _WIN32
// The test's PS_STUB_* settings are written into the process environment
// block, but this library's CRT keeps its OWN environment, so getenv() here
// cannot see them on Windows.  The process block is the shared one: read it
// with the OS call, never the CRT's.
const char* StubGetenv(const char* name) {
  static thread_local char value[4096];
  const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
  return length == 0 || length >= sizeof(value) ? nullptr : value;
}
#else
const char* StubGetenv(const char* name) { return getenv(name); }
#endif

long EnvOr(const char* name, long fallback) {
  const char* v = StubGetenv(name);
  if (v == nullptr || *v == '\0') {
    return fallback;
  }
  return strtol(v, nullptr, 10);
}

void FillDefaults(StubCacheConfig* config) {
  config->volume_path = nullptr;
  config->volume_size = 1ULL << 30;
  config->enable_checksum = 1;
  config->ram_cache_size = 8 << 20;  // deliberately NOT zero
  config->max_metadata_size = 4096;
}

// Layout-identical mirrors of the peer's published parameter structs.  Again
// declared here rather than included: this file has to be able to DISAGREE
// with the module's declaration, because that disagreement is the failure the
// module's handshake exists to catch.
struct StubWriteParams {
  size_t struct_size;
  uint8_t alternate_id;
  uint64_t content_length;
  uint32_t full_mask;
  int content_type;
  uint8_t flags;
  const char* origin_ct;
  const char* origin_etag;
  uint32_t cache_inserted_at;
  uint32_t origin_max_age;
  uint32_t origin_s_maxage;
  uint32_t origin_last_modified;
  uint16_t origin_cc_flags;
  uint8_t reserved_[6];
  // 1.13: the content binding.
  const uint8_t* origin_html_hash;
};

struct StubCacheControl {
  size_t struct_size;
  uint32_t max_age;
  uint32_t s_maxage;
  uint16_t cc_flags;
  uint8_t reserved_[2];
};

struct StubNotifyParams {
  size_t struct_size;
  const char* url;
  const char* hostname;
  const char* scheme;
  int content_type;
  uint32_t mask;
  int agent_request;
  const char* option_context;
  size_t option_context_length;
  const char* option_signature;
  const char* site;
  uint32_t notify_flags;
};

// The peer's published error codes, in its numbering.  Only the ones some
// caller distinguishes by value or a test steers a read into are here.
// Corrupted (3) and busy (6) deliberately carry no constant: their only
// role is ps_cache_read_alternate being asked to fail the way the real
// volume does -- neither found nor not-found -- and the tests steer that
// by literal value through PS_STUB_READ_ALT_STATUS, carrying their own
// named copies (daemon_serve_arm_test.cc).
constexpr int kOk = 0;
constexpr int kErrNotFound = 1;
constexpr int kErrIo = 2;
constexpr int kErrNoSpace = 4;
constexpr int kErrInvalidArg = 5;
constexpr int kErrClosed = 7;

constexpr uint8_t kSentinelOriginal = 0x0C;
constexpr uint64_t kOriginalContentCap = 16ULL * 1024 * 1024;
constexpr size_t kSignatureChars = 64;
constexpr size_t kSiteIdChars = 64;
constexpr uint32_t kNotifyFlagHostVouched = 0x1u;
constexpr uint32_t kNotifyFlagFromModule = 0x2u;
// The worker's llms.txt notification mask, which the module may not send.
constexpr uint32_t kLlmsTxtMask = 0xFFFFFFFDu;
constexpr size_t kMaxOptionContextBytes = 16384;

// The write-parameter sizes the peer has ever published.  A declared size is
// snapped DOWN to the largest of these that fits, which is why a struct that
// is merely close is read as the much shorter earlier revision.
size_t ClampWriteParamsSize(size_t declared) {
  const size_t v1_1 = offsetof(StubWriteParams, origin_etag);
  const size_t v1_2 = offsetof(StubWriteParams, origin_html_hash);
  if (declared >= sizeof(StubWriteParams)) {
    return sizeof(StubWriteParams);
  }
  if (declared >= v1_2) {
    return v1_2;
  }
  if (declared >= v1_1) {
    return v1_1;
  }
  return 0;
}

// NO C++ RUNTIME ANYWHERE IN THIS FILE, and it is not stylistic.
//
// This is a shared object that gets dlopen()ed and dlclose()d once per test.
// Linked with the C++ runtime it carries its own copy of that runtime's
// one-time state -- notably libstdc++'s ~72 KB emergency exception pool,
// allocated by a static constructor on every load and not returned on
// unload, because dlclose does not run the teardown that would free it. A
// leak checker sees one unattributable 72 KB allocation per load and fails
// whichever test loaded it, which is a defect in this file dressed as a
// defect in the code under test.
//
// So: no standard library (fixed buffers and C stdio), malloc/free rather
// than new/delete, and the link drops the C++ runtime outright. None of that
// costs anything here, and it keeps the object what it is meant to be -- an
// independently built peer with no runtime of its own.
void Log(const char* line) {
  const char* path = StubGetenv("PS_STUB_LOG");
  if (path == nullptr || *path == '\0') {
    return;
  }
  FILE* f = fopen(path, "a");
  if (f == nullptr) {
    return;
  }
  fputs(line, f);
  fputc('\n', f);
  fclose(f);
}

// Writes 2*len lowercase hex characters plus a NUL into `out`.
void Hex(const uint8_t* bytes, size_t len, char* out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kDigits[bytes[i] >> 4];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

// SHA-256, written out here rather than linked.
//
// The signature check below has to be a REAL one -- the peer recomputes a
// signature rather than trusting it, and a stand-in that skipped that would
// accept a pair the peer refuses. But this file is a dlopen()ed shared
// object, and pulling a crypto library into one costs a load-time allocation
// that outlives every unload and that a leak checker reports against the test
// binary. Forty lines of a published algorithm is the cheaper half of that
// trade, and it also keeps this file what it is meant to be: a peer built
// independently of everything around it.
uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  // Padded message length, computed rather than built: SHA-256 pads to a
  // multiple of 64 with a 0x80 byte, zeros, and a 64-bit big-endian bit
  // count.
  const size_t padded = ((len + 9 + 63) / 64) * 64;
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  auto byte_at = [&](size_t i) -> uint8_t {
    if (i < len) {
      return data[i];
    }
    if (i == len) {
      return 0x80;
    }
    if (i + 8 >= padded) {
      const int shift = static_cast<int>((padded - 1 - i) * 8);
      return static_cast<uint8_t>((bits >> shift) & 0xFF);
    }
    return 0;
  };

  for (size_t off = 0; off < padded; off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(byte_at(off + i * 4)) << 24) |
             (static_cast<uint32_t>(byte_at(off + i * 4 + 1)) << 16) |
             (static_cast<uint32_t>(byte_at(off + i * 4 + 2)) << 8) |
             static_cast<uint32_t>(byte_at(off + i * 4 + 3));
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 =
          Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 =
          Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
  }
}

// Writes kSignatureChars lowercase hex characters plus a NUL into `out`.
void Sha256Hex(const char* data, size_t len, char* out) {
  uint8_t digest[32];
  Sha256(reinterpret_cast<const uint8_t*>(data == nullptr ? "" : data), len,
         digest);
  Hex(digest, 32, out);
}

bool AsciiEqualsIgnoreCase(const char* a, size_t alen, const char* b) {
  const size_t blen = strlen(b);
  if (alen != blen) {
    return false;
  }
  for (size_t i = 0; i < alen; ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca | 0x20);
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb | 0x20);
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

// Everything one response's `Vary` decides on the store side, in ONE pass.
//
// Reproduced from the peer in this shape on purpose.  The peer exports two
// predicates over this value and documents them as thin wrappers over a single
// classification -- "so they cannot disagree about it", including the pairing
// that makes varies_accept imply storable.  A stand-in that computed them
// separately could pair them differently from the real library while every
// assertion over it stayed green, and the module RELIES on the pairing: its
// gate returns on a refusal before it ever asks the second question.
struct StubVaryVerdict {
  bool storable = true;
  bool varies_accept = false;
};

StubVaryVerdict ClassifyVaryForStore(const char* vary) {
  StubVaryVerdict verdict;
  if (vary == nullptr) {
    return verdict;  // No Vary at all: storable, negotiates on nothing.
  }
  // Exactly the four request fields the capability mask is derived from.
  // Anything else puts one representation behind a key that cannot tell the
  // variants apart.
  static const char* kAllowed[] = {"Accept-Encoding", "User-Agent", "Accept",
                                   "Save-Data"};
  const size_t len = strlen(vary);
  size_t pos = 0;
  while (pos <= len) {
    size_t comma = pos;
    while (comma < len && vary[comma] != ',') {
      ++comma;
    }
    size_t start = pos;
    size_t end = comma;
    // OWS is SP and HTAB only; nothing else is trimmed.
    while (start < end && (vary[start] == ' ' || vary[start] == '\t')) ++start;
    while (end > start && (vary[end - 1] == ' ' || vary[end - 1] == '\t'))
      --end;
    const size_t token_len = end - start;
    if (token_len != 0) {
      if (token_len == 1 && vary[start] == '*') {
        return StubVaryVerdict{false, false};
      }
      bool allowed = false;
      for (const char* candidate : kAllowed) {
        if (AsciiEqualsIgnoreCase(vary + start, token_len, candidate)) {
          allowed = true;
          break;
        }
      }
      if (!allowed) {
        // A refused response has nothing to store and so nothing to mark:
        // varies_accept is cleared even if `Accept` appeared earlier in the
        // list.  This is the peer's pairing rule, not a shortcut.
        return StubVaryVerdict{false, false};
      }
      if (AsciiEqualsIgnoreCase(vary + start, token_len, "Accept")) {
        verdict.varies_accept = true;
      }
    }
    if (comma >= len) {
      break;
    }
    pos = comma + 1;
  }
  return verdict;
}

// A single streaming write.  Carries the cap so the second of the peer's two
// enforcement positions -- the one on the write that crosses it -- exists
// here too, along with its sticky-error consequence.
struct StubWriteHandle {
  uint64_t cap = kOriginalContentCap;
  uint64_t written = 0;
  bool errored = false;
  bool over_cap = false;
  char url[512] = {0};
};

// ---------------------------------------------------------------------------
// The serve arm's model.
//
// WHAT THIS PART MODELS, AND WHY IT PLAYS BOTH GENERATIONS OF THE PEER'S
// FORMAT DISQUALIFY.  The
// selection the module's serve arm re-checks is one the peer performs, and
// the re-check exists because the peer's scorer used to award no bonus for a
// format mismatch and yet not REFUSE one -- a variant matching on the other
// axes still scored 200 and won over nothing, so a WebP-only client was handed
// AVIF. The peer's own later fix hard-disqualifies that case, which makes the
// module's disqualify DEFENSE IN DEPTH against the pinned daemon rather than
// the only thing standing between undecodable bytes and the wire.
//
// So the scorer below plays either generation and the DEFAULT IS THE OLDER
// ONE, deliberately: a stand-in that always disqualified would make the
// module's own disqualify unreachable and every test over it green by
// construction, which is precisely what the parity rig can no longer avoid at
// the current pin.  `PS_STUB_FORMAT_DISQUALIFY=1` switches on the current
// peer's format disqualify (its tie-break is not modelled -- no case here
// scores a positive tie), which is how the "no compatible variant exists, so
// the selection returns nothing and the arm reaches the durable original by id"
// path -- the shape a real family now presents -- is pinned in the suite. The
// SKIP of the durable-originals class is faithful in both modes: `read_best`
// can never return it, which is the trap the serve arm has to read around.
//
// The alternate table is declared by the test, one entry per comma:
//
//   PS_STUB_ALTERNATES="09:00:1,0a:00:1,0c:04:0"
//                       id:flags:worker_processed, all hex but the last
//
// Bodies are deterministic (a run of 'a' whose length is 16 + id) so a test
// can assert on the served length without a fixture.  The stored origin state
// every entry reports comes from PS_STUB_ORIGIN_*; there is one origin per
// process, because these tests are about selection and composition, not about
// per-entry origin state.
struct StubAlternate {
  uint8_t id;
  uint8_t flags;
  int worker_processed;
};

// Layout-identical mirrors of the peer's serve-side parameter structs.
// Declared here rather than included, for the same reason the write-side ones
// are: this file has to be able to disagree with the module's declaration.
struct StubFreshnessConfig {
  size_t struct_size;
  uint32_t max_age_cap;
  uint32_t immutable_max_age_cap;
  uint32_t html_max_age;
  uint32_t css_max_age;
  uint32_t image_max_age;
  uint8_t reserved_[4];
};

struct StubFreshnessInput {
  size_t struct_size;
  uint32_t now_seconds;
  uint32_t cache_inserted_at;
  uint32_t origin_max_age;
  uint32_t origin_s_maxage;
  int content_type;
  int cache_scope;
  int force_revalidate;
  uint16_t origin_cc_flags;
  uint8_t reserved_[2];
};

struct StubFreshnessResult {
  size_t struct_size;
  int verdict;
  uint32_t age_seconds;
  uint32_t effective_max_age;
  uint32_t remaining_ttl;
  int is_stale;
  int expired_by_age;
};

struct StubCacheControlInput {
  size_t struct_size;
  int mode;
  int cache_scope;
  uint32_t effective_max_age;
  uint32_t origin_max_age;
  int content_type;
  uint16_t origin_cc_flags;
  uint8_t synthesize_swr;
  uint8_t relay_origin_no_cache;
  uint8_t forward_origin_restrictions;
  uint8_t reserved_[5];
};

struct StubReadResult {
  StubAlternate alternate;
  char body[512];
  size_t body_length;
};

// The peer's alternate-id namespace: the low byte with viewport==3 is a
// sentinel rather than a capability vector.
bool StubIsSentinel(uint8_t id) { return ((id >> 2) & 0x03) == 0x03; }

size_t StubParseAlternates(StubAlternate* out, size_t capacity) {
  const char* spec = StubGetenv("PS_STUB_ALTERNATES");
  if (spec == nullptr || *spec == '\0') {
    return 0;
  }
  size_t count = 0;
  const char* p = spec;
  while (*p != '\0' && count < capacity) {
    char* end = nullptr;
    const long id = strtol(p, &end, 16);
    if (end == p) break;
    long flags = 0;
    long worker = 0;
    if (*end == ':') {
      p = end + 1;
      flags = strtol(p, &end, 16);
      if (*end == ':') {
        p = end + 1;
        worker = strtol(p, &end, 10);
      }
    }
    out[count].id = static_cast<uint8_t>(id);
    out[count].flags = static_cast<uint8_t>(flags);
    out[count].worker_processed = worker != 0 ? 1 : 0;
    ++count;
    while (*end == ',' || *end == ' ') ++end;
    p = end;
  }
  return count;
}

// The peer's ScoreAlternate, scoring arithmetic copied field for field.  The
// format arm plays EITHER generation of the peer's DISQUALIFY, selected by
// `PS_STUB_FORMAT_DISQUALIFY`, defaulting to the older one on purpose -- see
// the note above.  The other half of the peer's #1334 -- its deterministic
// tie-break -- is NOT modelled: the loop below keeps first-wins order, and no
// suite case scores a positive tie, so nothing here depends on it.
int StubScoreAlternate(uint32_t client_mask, uint32_t stored_mask) {
  if (StubIsSentinel(static_cast<uint8_t>(stored_mask & 0xFF))) {
    return 0;
  }
  const int client_format = static_cast<int>(client_mask & 0x03);
  const int stored_format = static_cast<int>(stored_mask & 0x03);
  const bool is_svg = stored_format == 3;
  int score = 0;
  if (is_svg) {
    score += 1200;
  } else if (client_format == stored_format) {
    score += 1000;
  } else if (stored_format == 0) {
    score += 100;
  } else if (EnvOr("PS_STUB_FORMAT_DISQUALIFY", 0) != 0) {
    // The CURRENT peer: a stored non-Original, non-SVG format that differs
    // from the one the client mask negotiated is hard-disqualified, exactly
    // as a mismatched non-identity encoding already was below.  With no
    // compatible member in the family the selection returns nothing at all.
    return 0;
  }
  if (is_svg || ((client_mask >> 2) & 0x03) == ((stored_mask >> 2) & 0x03)) {
    score += 80;
  }
  if (is_svg || ((client_mask >> 4) & 0x01) == ((stored_mask >> 4) & 0x01)) {
    score += 40;
  }
  if (is_svg && ((client_mask >> 5) & 0x01) == 1) {
    score += 50;
  } else if (((client_mask >> 5) & 0x01) == ((stored_mask >> 5) & 0x01)) {
    score += 20;
  }
  const int client_enc = static_cast<int>((client_mask >> 6) & 0x03);
  const int stored_enc = static_cast<int>((stored_mask >> 6) & 0x03);
  if (client_enc == stored_enc) {
    score += 60;
  } else if (stored_enc == 0) {
    score += 5;
  } else {
    return 0;
  }
  return score;
}

StubReadResult* StubMakeResult(const StubAlternate& alternate) {
  StubReadResult* result =
      static_cast<StubReadResult*>(calloc(1, sizeof(StubReadResult)));
  if (result == nullptr) {
    return nullptr;
  }
  result->alternate = alternate;
  result->body_length = 16u + alternate.id;
  if (result->body_length > sizeof(result->body)) {
    result->body_length = sizeof(result->body);
  }
  memset(result->body, 'a', result->body_length);
  // PS_STUB_BODY_MAGIC: the entry's leading bytes, hex, so a case can store
  // an entry whose CONTENT is a real image signature.  The peer fills a
  // converted-format alternate slot with the ORIGINAL bytes when the
  // conversion does not pay, so an entry's id and its content's format come
  // apart -- and a stand-in whose bodies are always a run of 'a' cannot put
  // a case on either side of that.
  const char* magic = StubGetenv("PS_STUB_BODY_MAGIC");
  if (magic != nullptr) {
    size_t i = 0;
    while (magic[0] != '\0' && magic[1] != '\0' && i < result->body_length) {
      char pair[3] = {magic[0], magic[1], '\0'};
      result->body[i++] = static_cast<char>(strtol(pair, nullptr, 16));
      magic += 2;
    }
  }
  // PS_STUB_GZIP_ISIZE: the length a gzip-coded entry's stream says it
  // compresses -- a gzip stream's last four bytes, little-endian.  The peer
  // writes real gzip streams for its compressed copies; a stand-in whose
  // bodies are a run of 'a' cannot put a case on either side of "compresses
  // the original" versus "compresses something smaller".  Applies to
  // entries whose id carries the gzip coding (bits 6-7 == 1) only.
  const long gzip_isize = EnvOr("PS_STUB_GZIP_ISIZE", -1);
  if (gzip_isize >= 0 && ((alternate.id >> 6) & 0x03) == 1 &&
      result->body_length >= 4) {
    const uint32_t value = static_cast<uint32_t>(gzip_isize);
    char* tail = result->body + result->body_length - 4;
    tail[0] = static_cast<char>(value & 0xFF);
    tail[1] = static_cast<char>((value >> 8) & 0xFF);
    tail[2] = static_cast<char>((value >> 16) & 0xFF);
    tail[3] = static_cast<char>((value >> 24) & 0xFF);
  }
  return result;
}

// PS_STUB_ORIGINAL_FLAGS_SEQUENCE: the flags byte the n-th read of the
// durable original reports.  The stub re-reads its environment on every call,
// so the last spec seen is remembered here and the count restarts when it
// changes.
void StubApplyOriginalFlagsSequence(StubAlternate* alternate) {
  static char last_spec[128] = "";
  static size_t reads = 0;
  const char* spec = StubGetenv("PS_STUB_ORIGINAL_FLAGS_SEQUENCE");
  if (spec == nullptr || spec[0] == '\0') {
    last_spec[0] = '\0';
    reads = 0;
    return;
  }
  // No C++ runtime in this file: a fixed buffer.  A spec too long to fit is
  // truncated, which only makes it compare as new on every call.
  char current[sizeof(last_spec)];
  snprintf(current, sizeof(current), "%s", spec);
  if (strcmp(last_spec, current) != 0 || strlen(spec) >= sizeof(current)) {
    memcpy(last_spec, current, sizeof(last_spec));
    reads = 0;
  }
  const size_t want = reads++;
  size_t index = 0;
  const char* p = spec;
  long value = alternate->flags;
  while (true) {
    char* end = nullptr;
    const long parsed = strtol(p, &end, 16);
    if (end == p) {
      break;
    }
    value = parsed;
    if (index == want) {
      break;
    }
    ++index;
    if (*end != ',') {
      break;
    }
    p = end + 1;
  }
  alternate->flags = static_cast<uint8_t>(value);
}

// The peer's site rule: NULL or "" is unsited; otherwise exactly
// kSiteIdChars lowercase hex characters, or the call is refused.
bool StubSiteIsValid(const char* site) {
  if (site == nullptr || *site == '\0') {
    return true;
  }
  const size_t length = strlen(site);
  if (length != kSiteIdChars) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    const char c = site[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// Whether a sited read may see the stored table: always, unless
// PS_STUB_SITE names the one keyspace the table belongs to.
bool StubSiteSeesEntries(const char* site) {
  const char* owner = StubGetenv("PS_STUB_SITE");
  if (owner == nullptr || *owner == '\0') {
    return true;
  }
  return site != nullptr && strcmp(site, owner) == 0;
}

const char* StubSiteForLog(const char* site) {
  return site == nullptr ? "(null)" : site;
}

// One line per sited cache call, "<op> site=<site> url=<url>", into the file
// PS_STUB_SITE_LOG names.  A separate file so the call log's lines keep the
// shape the older cases assert on.
void SiteLog(const char* op, const char* site, const char* url) {
  const char* path = StubGetenv("PS_STUB_SITE_LOG");
  if (path == nullptr || *path == '\0') {
    return;
  }
  FILE* f = fopen(path, "a");
  if (f == nullptr) {
    return;
  }
  fprintf(f, "%s site=%s url=%s\n", op, StubSiteForLog(site), url);
  fclose(f);
}

#ifdef PS_STUB_HTML_SERVE
// ---------------------------------------------------------------------------
// The HTML serve set's state: a few fixed slots, no C++ runtime (see above).
// ---------------------------------------------------------------------------

constexpr size_t kHashBytes = 32;
constexpr size_t kSidecarSlots = 8;
constexpr size_t kSidecarLines = 8;

struct StubSidecarWriteParams {
  size_t struct_size;
  const char* url;
  const char* hostname;
  const char* scheme;
  const char* site;
  const uint8_t* origin_html_hash;
};

struct StubServeRequest {
  size_t struct_size;
  int mode;
  const char* method;
  const char* accept;
  const char* cache_control;
  const char* pragma;
  const char* if_none_match;
  uint32_t request_flags;
  uint32_t now_seconds;
};

// The size a 1.15 request has (the 1.14 one plus scope, class, cap, a
// reserved word and the stamp).
constexpr size_t kServeRequestSize15 = 88;

struct StubStoredFacts {
  size_t struct_size;
  uint32_t flags;
  uint32_t stored_at;
  uint32_t age_at_store;
  uint32_t lifetime_s;
  uint32_t render_ttl_s;
  int withheld_reason;
  uint8_t binding_hash[kHashBytes];
  uint8_t markdown_hash[kHashBytes];
};

struct StubServeVerdict {
  size_t struct_size;
  int action;
  int reason;
  int enforce;
  int wants_markdown;
  uint32_t age_s;
  int not_modified;
  char etag[32];
#ifdef PS_STUB_HTML_SERVE_16
  int markdown_encoding;
  int reserved_1_16;
#endif
};

#ifdef PS_STUB_HTML_SERVE_16
// The 1.16 request's coding word (the 1.15 reserved one) and its bits.
constexpr size_t kServeRequestCodingOffset = 76;
constexpr uint32_t kServeRequestIdentityOk = 1024u;
constexpr uint32_t kServeRequestEncodingNone = 2048u;
constexpr size_t kServeVerdictSize16 = 72;
#endif

// A stored sidecar, and also the block a read hands out (a malloc'd copy).
struct StubSidecar {
  int used;
  char url[512];
  char site[kSiteIdChars + 1];
  size_t count;
  char names[kSidecarLines][64];
  char values[kSidecarLines][256];
  int has_hash;
  uint8_t hash[kHashBytes];
};

StubSidecar g_sidecars[kSidecarSlots];

struct StubEpoch {
  int used;
  char site[kSiteIdChars + 1];
  uint64_t value;
};

StubEpoch g_epochs[kSidecarSlots];

// A site the sited-only entry points accept: exactly kSiteIdChars hex.
bool StubSiteIsNamed(const char* site) {
  return site != nullptr && *site != '\0' && StubSiteIsValid(site);
}

StubSidecar* FindSidecar(const char* url, const char* site) {
  for (size_t i = 0; i < kSidecarSlots; ++i) {
    StubSidecar* s = &g_sidecars[i];
    if (s->used && strcmp(s->url, url) == 0 &&
        strcmp(s->site, site == nullptr ? "" : site) == 0) {
      return s;
    }
  }
  return nullptr;
}

bool AllZero(const uint8_t* bytes) {
  for (size_t i = 0; i < kHashBytes; ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

const char* TrimLeft(const char* s) {
  while (*s == ' ' || *s == '\t') ++s;
  return s;
}

// Whether [begin, end) equals `etag`, both compared without a W/ prefix.
bool WeakEqual(const char* begin, const char* end, const char* etag) {
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
  if (end - begin >= 2 && begin[0] == 'W' && begin[1] == '/') begin += 2;
  if (etag[0] == 'W' && etag[1] == '/') etag += 2;
  const size_t length = static_cast<size_t>(end - begin);
  return strlen(etag) == length && strncmp(begin, etag, length) == 0;
}
#endif  // PS_STUB_HTML_SERVE

}  // namespace

extern "C" {

int ps_version_major() { return static_cast<int>(EnvOr("PS_STUB_MAJOR", 1)); }
int ps_version_minor() { return static_cast<int>(EnvOr("PS_STUB_MINOR", 12)); }

// The size-unaware spelling: writes its own struct's worth, and stamps its
// own sizeof -- which is what makes a layout disagreement detectable.
void ps_cache_config_init(StubCacheConfig* config) {
  if (config == nullptr) return;
  const size_t claimed = static_cast<size_t>(
      EnvOr("PS_STUB_STRUCT_SIZE", static_cast<long>(sizeof(StubCacheConfig))));
  memset(config, 0, sizeof(StubCacheConfig));
  FillDefaults(config);
  config->struct_size = claimed;
}

#ifndef PS_STUB_OMIT_SIZED_INIT
// The size-aware spelling: records the size it was GIVEN and writes nothing
// past it -- unless the test asks it to misbehave, which is how the loader's
// slack check gets a failing case.
void ps_cache_config_init_sized(StubCacheConfig* config, size_t size) {
  if (config == nullptr || size < sizeof(size_t)) return;
  const size_t writes =
      size < sizeof(StubCacheConfig) ? size : sizeof(StubCacheConfig);
  memset(config, 0, writes);
  if (writes == sizeof(StubCacheConfig)) {
    FillDefaults(config);
  }
  config->struct_size = size;

  const size_t scribble = static_cast<size_t>(EnvOr("PS_STUB_SCRIBBLE", 0));
  if (scribble > 0) {
    memset(reinterpret_cast<unsigned char*>(config) + size, 0x5C, scribble);
  }
}
#endif  // PS_STUB_OMIT_SIZED_INIT

uint64_t ps_read_shared_config_volume_size(const char* cache_path) {
  return static_cast<uint64_t>(EnvOr("PS_STUB_VOLUME_SIZE", 0));
}

#ifndef PS_STUB_OMIT_GENERATION
// The cache-directory generation (cache_dir_generation), published since the
// daemon's privilege drop. Bound OPTIONALLY by the module: the omit flavour
// below is the pre-H1 daemon, whose absence must still load. The default is
// the current generation (DaemonAdapter::kCacheDirGeneration), so a module
// test attaching through this stub sees a daemon it agrees with.
uint32_t ps_read_shared_config_generation(const char* cache_path) {
  return static_cast<uint32_t>(EnvOr("PS_STUB_GENERATION", 3));
}
#endif  // PS_STUB_OMIT_GENERATION

#ifdef PS_STUB_HTML_POLICY
// The HTML cache policy (1.12), in the flavour that exports it.  Answers what
// PS_STUB_HTML_ACTION and PS_STUB_HTML_REASON say; the rules themselves are
// the real library's.  The formatter writes `key=value` pairs so a test can
// read the fields back without a JSON parser.
int ps_html_store_classify(int, const char* method, const char* url_path,
                           uint32_t, int, const char* const*,
                           const char* const*, size_t, uint64_t, int,
                           const char*, size_t, uint64_t, uint32_t,
                           int* out_reason, uint32_t* out_lifetime_s,
                           uint32_t* out_flags, uint16_t* out_cc_flags,
                           int* out_sidecar, char* detail_buf,
                           size_t detail_capacity, size_t* out_detail_len) {
  if (method == nullptr || url_path == nullptr) return -1;
  const int action = static_cast<int>(EnvOr("PS_STUB_HTML_ACTION", 3));
  if (action < 0) return action;
  const int reason = static_cast<int>(EnvOr("PS_STUB_HTML_REASON", 0));
  *out_reason = reason;
  *out_lifetime_s = 60;
  *out_flags = 1u | 16u | 32u;
  *out_cc_flags = 0;
  *out_sidecar = 0;
  const char* detail = reason == 18 ? "meta_csrf_token" : "";
  *out_detail_len = std::strlen(detail);
  if (detail_capacity > 0) {
    std::snprintf(detail_buf, detail_capacity, "%s", detail);
  }
  return action;
}

int ps_html_decision_format(const char* const* keys, const char* const* values,
                            size_t count, char* buf, size_t capacity,
                            size_t* out_len) {
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    const int n =
        std::snprintf(buf + used, used < capacity ? capacity - used : 0,
                      "%s=%s ", keys[i], values[i]);
    if (n < 0) return -2;
    used += static_cast<size_t>(n);
  }
  *out_len = used;
  return used + 1 > capacity ? -3 : 0;
}

const char* ps_html_store_reason_name(int reason) {
  if (reason == 0) return "none";
  if (reason == 18) return "personalisation_signal";
  return nullptr;
}
#endif  // PS_STUB_HTML_POLICY

#ifdef PS_STUB_HTML_SERVE
// ---------------------------------------------------------------------------
// The HTML serve set (1.13 + 1.14), in the flavour that exports it.  Every
// entry point keeps the real library's argument checks and fail directions;
// the verdicts themselves are what the PS_STUB_HTML_SERVE_* settings say.
// ---------------------------------------------------------------------------

int ps_wants_agent_markdown(const char* accept) {
  return accept != nullptr && strstr(accept, "text/markdown") != nullptr;
}

int ps_read_shared_config_agent_entitled(const char* cache_path) {
  if (cache_path == nullptr) return -1;
  return static_cast<int>(EnvOr("PS_STUB_AGENT_ENTITLED", 1));
}

int ps_read_shared_config_html_mode(const char* cache_path) {
  if (cache_path == nullptr) return -1;
  return static_cast<int>(EnvOr("PS_STUB_HTML_MODE", 1));
}

const char* ps_html_serve_reason_name(int reason) {
  static const char* const kNames[] = {"fresh",
                                       "mode",
                                       "auth_request",
                                       "cookie_request",
                                       "client_no_cache",
                                       "needs_validation",
                                       "stale",
                                       "headers_not_reproducible",
                                       "sidecar_missing",
                                       "purged",
                                       "absent",
                                       "binding_stale",
                                       "parent_stale",
                                       "method",
                                       "range",
                                       "cors",
                                       "no_transform",
                                       "withheld",
                                       "skew"
#ifdef PS_STUB_HTML_SERVE_15
                                       ,
                                       "scope",
                                       "capped",
                                       "conditional",
                                       "credentialed",
                                       "legacy_entry",
                                       "policy_mismatch",
                                       "request_header",
                                       "path"
#endif
#ifdef PS_STUB_HTML_SERVE_16
                                       ,
                                       "no_acceptable_encoding"
#endif
  };
  if (reason < 0 ||
      static_cast<size_t>(reason) >= sizeof(kNames) / sizeof(kNames[0])) {
    return nullptr;
  }
  return kNames[reason];
}

int ps_html_markdown_etag(const uint8_t* binding_hash,
                          const uint8_t* markdown_hash, char* buf,
                          size_t capacity, size_t* out_len) {
  if (binding_hash == nullptr || markdown_hash == nullptr ||
      out_len == nullptr || (buf == nullptr && capacity != 0) ||
      AllZero(binding_hash) || AllZero(markdown_hash)) {
    return -1;
  }
  uint8_t both[2 * kHashBytes];
  memcpy(both, binding_hash, kHashBytes);
  memcpy(both + kHashBytes, markdown_hash, kHashBytes);
  uint8_t digest[32];
  Sha256(both, sizeof(both), digest);
  char hex[2 * 8 + 1];
  Hex(digest, 8, hex);
  char tag[40];
  const int n = snprintf(tag, sizeof(tag), "W/\"psm-%s\"", hex);
  *out_len = static_cast<size_t>(n);
  if (capacity <= static_cast<size_t>(n)) {
    if (capacity > 0) buf[0] = '\0';
    return -3;
  }
  memcpy(buf, tag, static_cast<size_t>(n) + 1);
  return 0;
}

int ps_if_none_match_matches_weak(const char* if_none_match, const char* etag) {
  if (etag == nullptr || *etag == '\0') return -1;
  if (if_none_match == nullptr) return 0;
  const char* p = TrimLeft(if_none_match);
  const char* end = p + strlen(p);
  while (end > p && (end[-1] == ' ' || end[-1] == '\t')) --end;
  if (end - p == 1 && *p == '*') return 1;
  while (p < end) {
    const char* comma = p;
    while (comma < end && *comma != ',') ++comma;
    if (WeakEqual(TrimLeft(p), comma, etag)) return 1;
    p = comma < end ? comma + 1 : end;
  }
  return 0;
}

#ifdef PS_STUB_HTML_SERVE_16
int ps_html_markdown_etag_coded(const uint8_t* binding_hash,
                                const uint8_t* markdown_hash, int coding,
                                char* buf, size_t capacity, size_t* out_len);
#endif

int ps_html_serve_classify(const StubServeRequest* request,
                           const StubStoredFacts* facts,
                           StubServeVerdict* out) {
  if (request == nullptr || facts == nullptr || out == nullptr ||
      request->struct_size < sizeof(StubServeRequest) ||
      facts->struct_size < sizeof(StubStoredFacts) ||
      out->struct_size < sizeof(size_t) || request->mode < 0 ||
      request->mode > 3 || request->method == nullptr) {
    return -1;
  }
  StubServeVerdict v;
  memset(&v, 0, sizeof(v));
  v.wants_markdown = ps_wants_agent_markdown(request->accept);
#ifdef PS_STUB_HTML_SERVE_15
  // A 1.15 library reads the 1.15 fields as zero from a shorter request:
  // scope agents, class unknown, which refuses serve-cached-html.
  const bool short_cached_html =
      request->mode == 3 && request->struct_size < kServeRequestSize15;
#else
  const bool short_cached_html = false;
#endif
  if (request->mode < 2) {
    v.reason = 1;  // mode
  } else if (short_cached_html) {
    v.reason = 19;  // scope
    v.enforce = 1;
  } else if ((facts->flags & 0x1u) == 0) {
    v.reason = 10;  // absent
    v.enforce = 1;
  } else {
    v.enforce = 1;
    v.action = static_cast<int>(EnvOr("PS_STUB_HTML_SERVE_ACTION", 0));
    v.reason = static_cast<int>(EnvOr("PS_STUB_HTML_SERVE_REASON", 0));
    size_t len = 0;
#ifdef PS_STUB_HTML_SERVE_16
    // The coding, only for a caller whose request and verdict carry it.
    if (v.action == 2 && request->struct_size >= kServeRequestSize15 &&
        out->struct_size >= kServeVerdictSize16) {
      uint32_t coding = 0;
      memcpy(&coding,
             reinterpret_cast<const char*>(request) + kServeRequestCodingOffset,
             sizeof(coding));
      const bool bound = (coding == 1 && (facts->flags & 0x4000u) != 0) ||
                         (coding == 2 && (facts->flags & 0x8000u) != 0);
      if ((request->request_flags & kServeRequestEncodingNone) != 0) {
        v.action = 0;
        v.reason = 27;  // no_acceptable_encoding
      } else if (coding == 1 || coding == 2) {
        if (bound) {
          v.markdown_encoding = static_cast<int>(coding);
        } else if ((request->request_flags & kServeRequestIdentityOk) == 0) {
          v.action = 0;
          v.reason = 27;
        }
      }
    }
    if (v.action == 2 &&
        ps_html_markdown_etag_coded(facts->binding_hash, facts->markdown_hash,
                                    v.markdown_encoding, v.etag, sizeof(v.etag),
                                    &len) == 0) {
      v.not_modified =
          ps_if_none_match_matches_weak(request->if_none_match, v.etag) == 1;
    }
#else
    if (v.action == 2 &&
        ps_html_markdown_etag(facts->binding_hash, facts->markdown_hash, v.etag,
                              sizeof(v.etag), &len) == 0) {
      v.not_modified =
          ps_if_none_match_matches_weak(request->if_none_match, v.etag) == 1;
    }
#endif
  }
  if (EnvOr("PS_STUB_ECHO_SIZES", 0) != 0) {
    v.age_s = static_cast<uint32_t>(request->struct_size);
    v.not_modified = static_cast<int>(facts->struct_size);
  }
  const size_t size = out->struct_size;
  v.struct_size = size;
  memcpy(out, &v, size < sizeof(v) ? size : sizeof(v));
  return 0;
}

int ps_html_store_write_params(int action, uint32_t lifetime_s,
                               uint32_t store_flags, uint16_t cc_flags,
                               int sidecar_stored, int64_t capture_start_ms,
                               const char* const* age_lines, size_t age_count,
                               uint64_t content_length,
                               const char* origin_content_type,
                               StubWriteParams* params) {
  if ((action != 1 && action != 2) || capture_start_ms < 1000 ||
      params == nullptr || origin_content_type == nullptr ||
      (age_lines == nullptr && age_count != 0) ||
      params->struct_size < offsetof(StubWriteParams, origin_html_hash)) {
    return -1;
  }
  const int64_t capture_s = capture_start_ms / 1000;
  long age = -1;
  for (size_t i = 0; i < age_count; ++i) {
    const char* line = age_lines[i];
    if (line == nullptr || *line == '\0') return -1;
    char* end = nullptr;
    const long value = strtol(line, &end, 10);
    if (*end != '\0' || value < 0 || (age >= 0 && age != value)) return -1;
    age = value;
  }
  if (age < 0) age = 0;
  if (age >= capture_s) return -1;
  params->alternate_id = kSentinelOriginal;
  params->full_mask = kSentinelOriginal;
  params->content_type = 0;  // HTML
  params->content_length = content_length;
  params->origin_ct = origin_content_type;
  params->origin_max_age = lifetime_s;
  params->origin_cc_flags = cc_flags;
  params->flags = static_cast<uint8_t>(0x10u | (sidecar_stored ? 0x20u : 0u));
  params->cache_inserted_at = static_cast<uint32_t>(capture_s - age);
  (void)store_flags;
  return 0;
}

int ps_html_stored_facts_from_reads(const void* original, const void* markdown,
                                    const void* withheld_marker,
                                    int sidecar_present, StubStoredFacts* out) {
  if (out == nullptr || out->struct_size < sizeof(StubStoredFacts)) {
    return -1;
  }
  const size_t size = out->struct_size;
  memset(out, 0, sizeof(*out));
  out->struct_size = size;
  if (original == nullptr) return 0;
  out->flags = 0x1u;
  if (sidecar_present) out->flags |= 0x20u;
  if (markdown != nullptr) out->flags |= 0x100u;
  if (withheld_marker != nullptr) {
    out->flags |= 0x800u;
  } else if (markdown != nullptr) {
    out->flags |= 0x200u;
  }
  return 0;
}

#ifdef PS_STUB_HTML_SERVE_15
struct StubStoredFacts15 {
  StubStoredFacts base;
  uint64_t entry_policy_stamp;
};

int ps_header_block_policy_stamp(const void* block, uint64_t* out);

int ps_html_stored_facts_from_reads_ex(const void* original,
                                       const void* markdown,
                                       const void* withheld_marker,
                                       int sidecar_present, const void* block,
                                       StubStoredFacts15* out) {
  if (out == nullptr || out->base.struct_size < sizeof(StubStoredFacts)) {
    return -1;
  }
  const size_t size = out->base.struct_size;
  if (ps_html_stored_facts_from_reads(original, markdown, withheld_marker,
                                      sidecar_present, &out->base) != 0) {
    return -1;
  }
  if (size >= sizeof(StubStoredFacts15)) {
    uint64_t stamp = 0;
    if (block != nullptr) ps_header_block_policy_stamp(block, &stamp);
    out->entry_policy_stamp = stamp;
  }
  return 0;
}
#endif  // PS_STUB_HTML_SERVE_15

int ps_cache_write_headers_sidecar_site(void* cache,
                                        const StubSidecarWriteParams* params,
                                        const char* const* names,
                                        const char* const* values, size_t count,
                                        int* out_verdict, int* out_stored) {
  if (cache == nullptr || params == nullptr ||
      params->struct_size < sizeof(StubSidecarWriteParams) ||
      params->url == nullptr || params->hostname == nullptr ||
      !StubSiteIsValid(params->site) ||
      (params->origin_html_hash != nullptr &&
       AllZero(params->origin_html_hash)) ||
      (count != 0 && (names == nullptr || values == nullptr)) ||
      count > kSidecarLines ||
      strlen(params->url) >= sizeof(g_sidecars[0].url)) {
    return kErrInvalidArg;
  }
  for (size_t i = 0; i < count; ++i) {
    if (names[i] == nullptr || values[i] == nullptr) return kErrInvalidArg;
  }
  const long forced = EnvOr("PS_STUB_SIDECAR_WRITE_STATUS", 0);
  if (forced != 0) return static_cast<int>(forced);
  SiteLog("write_sidecar", params->site, params->url);
  if (out_verdict != nullptr) *out_verdict = 0;
  const bool paired = params->origin_html_hash != nullptr;
  if (count == 0 && !paired) {
    if (out_stored != nullptr) *out_stored = 0;
    return kOk;
  }
  StubSidecar* slot = FindSidecar(params->url, params->site);
  for (size_t i = 0; slot == nullptr && i < kSidecarSlots; ++i) {
    if (!g_sidecars[i].used) slot = &g_sidecars[i];
  }
  if (slot == nullptr) return kErrNoSpace;
  memset(slot, 0, sizeof(*slot));
  slot->used = 1;
  snprintf(slot->url, sizeof(slot->url), "%s", params->url);
  snprintf(slot->site, sizeof(slot->site), "%s",
           params->site == nullptr ? "" : params->site);
  slot->count = count;
  for (size_t i = 0; i < count; ++i) {
    snprintf(slot->names[i], sizeof(slot->names[i]), "%s", names[i]);
    snprintf(slot->values[i], sizeof(slot->values[i]), "%s", values[i]);
  }
  if (paired) {
    slot->has_hash = 1;
    memcpy(slot->hash, params->origin_html_hash, kHashBytes);
  }
  if (out_stored != nullptr) *out_stored = 1;
  return kOk;
}

int ps_headers_sidecar_read(void* cache, const char* url, const char* hostname,
                            const char* scheme, const char* site, void** out) {
  if (out != nullptr) *out = nullptr;
  if (cache == nullptr || url == nullptr || hostname == nullptr ||
      out == nullptr || !StubSiteIsValid(site)) {
    return kErrInvalidArg;
  }
  SiteLog("read_sidecar", site, url);
  const StubSidecar* found = FindSidecar(url, site);
  if (found == nullptr) return kErrNotFound;
  StubSidecar* block = static_cast<StubSidecar*>(malloc(sizeof(StubSidecar)));
  if (block == nullptr) return kErrIo;
  memcpy(block, found, sizeof(*block));
  *out = block;
  return kOk;
}

size_t ps_header_block_count(const void* block) {
  return block == nullptr ? 0 : static_cast<const StubSidecar*>(block)->count;
}

int ps_header_block_get(const void* block, size_t index, const char** out_name,
                        size_t* out_name_len, const char** out_value,
                        size_t* out_value_len) {
  const StubSidecar* b = static_cast<const StubSidecar*>(block);
  if (b == nullptr || index >= b->count) return kErrInvalidArg;
  if (out_name != nullptr) *out_name = b->names[index];
  if (out_name_len != nullptr) *out_name_len = strlen(b->names[index]);
  if (out_value != nullptr) *out_value = b->values[index];
  if (out_value_len != nullptr) *out_value_len = strlen(b->values[index]);
  return kOk;
}

const uint8_t* ps_header_block_pairing_hash(const void* block) {
  const StubSidecar* b = static_cast<const StubSidecar*>(block);
  return b == nullptr || !b->has_hash ? nullptr : b->hash;
}

int ps_header_block_pairing(const void* block, const uint8_t* entry_hash) {
  const StubSidecar* b = static_cast<const StubSidecar*>(block);
  if (b == nullptr) return -1;
  return b->has_hash && entry_hash != nullptr &&
                 memcmp(b->hash, entry_hash, kHashBytes) == 0
             ? 1
             : 0;
}

void ps_header_block_free(void* block) { free(block); }

#ifdef PS_STUB_HTML_SERVE_15
// ---------------------------------------------------------------------------
// The 1.15 set, in the flavour that exports it.  The argument checks are the
// real library's; the rules are a small, fixed subset of them, enough for a
// test to see each answer reach the caller.
// ---------------------------------------------------------------------------

struct StubRequestGuard {
  size_t struct_size;
  int reason;
  int header_index;
  int xfp;
};

struct StubCookieSpan {
  size_t line;
  size_t offset;
  size_t length;
};

struct StubCookieClass {
  size_t struct_size;
  int cookie_class;
  int malformed;
  StubCookieSpan first_blocking;
  size_t ignorable_count;
};

static bool StubCaseEqual(const char* a, const char* b) {
  for (; *a != '\0' && *b != '\0'; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return *a == *b;
}

// Refuses a `;` in the path, an X-Forwarded-Host line, and an
// X-Forwarded-Proto that is not the keyed scheme.
int ps_html_request_guard(const char* raw_path, const char* const* names,
                          const char* const* values, size_t count,
                          const char* keyed_scheme, StubRequestGuard* out) {
  if (raw_path == nullptr || keyed_scheme == nullptr || out == nullptr ||
      out->struct_size < sizeof(StubRequestGuard) ||
      (count > 0 && (names == nullptr || values == nullptr))) {
    return -1;
  }
  out->reason = 0;
  out->header_index = -1;
  out->xfp = 0;
  const char* query = strchr(raw_path, '?');
  const char* semi = strchr(raw_path, ';');
  if (semi != nullptr && (query == nullptr || semi < query)) {
    out->reason = 2;  // path
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    if (names[i] == nullptr || values[i] == nullptr) return -1;
    if (StubCaseEqual(names[i], "X-Forwarded-Host")) {
      out->reason = 1;
      out->header_index = static_cast<int>(i);
      return 0;
    }
    if (StubCaseEqual(names[i], "X-Forwarded-Proto") &&
        !StubCaseEqual(values[i], keyed_scheme)) {
      out->reason = 1;
      out->header_index = static_cast<int>(i);
      out->xfp = 1;
      return 0;
    }
  }
  return 0;
}

// One Cookie line of `name=value` pairs: `_ga` and `_gid` are ignorable,
// anything else blocks.
int ps_html_classify_cookies(const char* const* lines, size_t line_count,
                             int ignore_builtin, const char* const* additions,
                             size_t addition_count,
                             StubCookieSpan* ignorable_values,
                             size_t ignorable_capacity, StubCookieClass* out) {
  if ((line_count > 0 && lines == nullptr) ||
      (addition_count > 0 && additions == nullptr) || out == nullptr ||
      out->struct_size < sizeof(StubCookieClass) ||
      (ignorable_values == nullptr && ignorable_capacity != 0)) {
    return -1;
  }
  out->cookie_class = 0;
  out->malformed = 0;
  memset(&out->first_blocking, 0, sizeof(out->first_blocking));
  out->ignorable_count = 0;
  for (size_t l = 0; l < line_count; ++l) {
    const char* line = lines[l];
    if (line == nullptr) return -1;
    size_t pos = 0;
    const size_t n = strlen(line);
    while (pos < n) {
      while (pos < n && (line[pos] == ' ' || line[pos] == ';')) ++pos;
      if (pos >= n) break;
      const size_t name_at = pos;
      while (pos < n && line[pos] != '=' && line[pos] != ';') ++pos;
      if (pos >= n || line[pos] != '=') {
        out->cookie_class = 2;
        out->malformed = 1;
        return 0;
      }
      const size_t name_len = pos - name_at;
      ++pos;
      const size_t value_at = pos;
      while (pos < n && line[pos] != ';') ++pos;
      const bool listed =
          ignore_builtin != 0 &&
          ((name_len == 3 && strncmp(line + name_at, "_ga", 3) == 0) ||
           (name_len == 4 && strncmp(line + name_at, "_gid", 4) == 0));
      if (!listed) {
        out->cookie_class = 2;
        out->first_blocking.line = l;
        out->first_blocking.offset = name_at;
        out->first_blocking.length = name_len;
        return 0;
      }
      if (out->ignorable_count < ignorable_capacity) {
        StubCookieSpan* v = &ignorable_values[out->ignorable_count];
        v->line = l;
        v->offset = value_at;
        v->length = pos - value_at;
      }
      ++out->ignorable_count;
      out->cookie_class = 1;
    }
  }
  if (out->ignorable_count > ignorable_capacity) {
    out->cookie_class = 2;
    memset(&out->first_blocking, 0, sizeof(out->first_blocking));
  }
  return 0;
}

// Refuses `*` alone and a prefix shorter than 3 characters; warns for any
// accepted prefix.
int ps_html_ignore_cookie_guard(const char* addition, int* out_warning) {
  if (addition == nullptr) return -1;
  const size_t n = strlen(addition);
  const bool prefix = n > 0 && addition[n - 1] == '*';
  if (out_warning != nullptr) *out_warning = 0;
  if (n == 0 || (prefix && n - 1 < 3)) return 0;
  if (prefix && out_warning != nullptr) *out_warning = 1;
  return 1;
}

// Not the real hash: credentialed, the switch, the version and the number of
// additions, offset so it is never zero.
int ps_html_policy_stamp(int credentialed, int ignore_builtin,
                         uint32_t list_version, const char* const* additions,
                         size_t addition_count, uint64_t* out) {
  if ((credentialed != 0 && credentialed != 1) || out == nullptr ||
      (addition_count > 0 && additions == nullptr)) {
    return -1;
  }
  *out = 0x1000000ull + static_cast<uint64_t>(credentialed) * 0x100000ull +
         static_cast<uint64_t>(ignore_builtin != 0) * 0x10000ull +
         static_cast<uint64_t>(list_version) * 0x100ull + addition_count;
  return 0;
}

#ifndef PS_STUB_OMIT_UNSAFE_METHOD_INVALIDATES
int ps_html_unsafe_method_invalidates(const char* method, int status) {
  if (status < 200 || status > 399) return 0;
  if (method == nullptr || *method == '\0') return 1;
  static const char* const kSafe[] = {"GET", "HEAD", "OPTIONS", "TRACE"};
  for (size_t i = 0; i < sizeof(kSafe) / sizeof(kSafe[0]); ++i) {
    if (strcmp(method, kSafe[i]) == 0) return 0;
  }
  return 1;
}
#endif  // PS_STUB_OMIT_UNSAFE_METHOD_INVALIDATES

// A stub block carries no stamp unless PS_STUB_POLICY_STAMP names one.
int ps_header_block_policy_stamp(const void* block, uint64_t* out) {
  if (block == nullptr || out == nullptr) return -1;
  const long stamp = EnvOr("PS_STUB_POLICY_STAMP", 0);
  *out = static_cast<uint64_t>(stamp);
  return stamp != 0 ? 1 : 0;
}

#ifdef PS_STUB_HTML_SERVE_16
// ---------------------------------------------------------------------------
// The 1.16 set.  The argument checks are the real library's; the coding
// choice is a small subset of its rules (br, gzip, x-gzip, identity, `*`,
// q=0 and a q of up to three decimals), enough for a test to see an order
// come back.
// ---------------------------------------------------------------------------

namespace {

bool StubCaseEqualN(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// The q of one element ("" = 1000), in thousandths; -1 when it does not
// parse.
int StubQ(const char* params, size_t length) {
  int q = 1000;
  size_t i = 0;
  while (i < length) {
    while (i < length && (params[i] == ' ' || params[i] == ';')) ++i;
    if (i + 1 < length && (params[i] == 'q' || params[i] == 'Q') &&
        params[i + 1] == '=') {
      i += 2;
      if (i >= length || (params[i] != '0' && params[i] != '1')) return -1;
      int whole = params[i] - '0';
      ++i;
      int frac = 0;
      int digits = 0;
      if (i < length && params[i] == '.') {
        ++i;
        while (i < length && params[i] >= '0' && params[i] <= '9' &&
               digits < 3) {
          frac = frac * 10 + (params[i] - '0');
          ++digits;
          ++i;
        }
      }
      while (digits < 3) {
        frac *= 10;
        ++digits;
      }
      q = whole * 1000 + frac;
      if (q > 1000) return -1;
      while (i < length && params[i] == ' ') ++i;
      if (i < length && params[i] != ';') return -1;
    } else {
      while (i < length && params[i] != ';') ++i;
    }
  }
  return q;
}

}  // namespace

#ifndef PS_STUB_OMIT_MARKDOWN_SELECT_ENCODING
int ps_markdown_select_encoding(const char* const* lines, size_t count,
                                int* out_order) {
  if (out_order == nullptr || (lines == nullptr && count != 0)) return -1;
  // q per coding: identity 0, gzip 1, br 2; -2 = not named.
  int q[3] = {-2, -2, -2};
  int star = -2;
  bool any = false;
  for (size_t l = 0; l < count; ++l) {
    const char* line = lines[l];
    if (line == nullptr) continue;
    const char* p = line;
    while (*p != '\0') {
      while (*p == ' ' || *p == ',') ++p;
      if (*p == '\0') break;
      const char* end = p;
      while (*end != '\0' && *end != ',') ++end;
      const char* semi = p;
      while (semi < end && *semi != ';') ++semi;
      const char* name_end = semi;
      while (name_end > p && name_end[-1] == ' ') --name_end;
      const size_t n = static_cast<size_t>(name_end - p);
      const int value = StubQ(semi, static_cast<size_t>(end - semi));
      any = true;
      if (value >= 0) {
        int* slot = nullptr;
        if (n == 1 && *p == '*') {
          slot = &star;
        } else if (n == 2 && StubCaseEqualN(p, "br", 2)) {
          slot = &q[2];
        } else if ((n == 4 && StubCaseEqualN(p, "gzip", 4)) ||
                   (n == 6 && StubCaseEqualN(p, "x-gzip", 6))) {
          slot = &q[1];
        } else if (n == 8 && StubCaseEqualN(p, "identity", 8)) {
          slot = &q[0];
        }
        if (slot != nullptr && (*slot == -2 || value < *slot)) *slot = value;
      }
      p = end;
    }
  }
  if (!any) {
    out_order[0] = 0;
    return 1;
  }
  for (int c = 1; c <= 2; ++c) {
    if (q[c] == -2) q[c] = star == -2 ? 0 : star;
  }
  // Identity is acceptable unless refused by name or by `*;q=0`; unnamed,
  // it ranks after every coding named.
  int identity = q[0];
  if (identity == -2) identity = star == 0 ? 0 : -1;
  int n = 0;
  for (int pass = 1000; pass >= 1; --pass) {
    if (q[2] == pass) out_order[n++] = 2;
    if (q[1] == pass) out_order[n++] = 1;
    if (identity == pass) out_order[n++] = 0;
  }
  if (identity == -1) out_order[n++] = 0;
  return n;
}
#endif  // PS_STUB_OMIT_MARKDOWN_SELECT_ENCODING

int ps_html_stored_facts_from_reads_coded(const void* original,
                                          const void* markdown,
                                          const void* withheld_marker,
                                          int sidecar_present,
                                          const void* block, const void* coded,
                                          int coding, StubStoredFacts15* out) {
  if (coded != nullptr && coding != 1 && coding != 2) return -1;
  if (ps_html_stored_facts_from_reads_ex(original, markdown, withheld_marker,
                                         sidecar_present, block, out) != 0) {
    return -1;
  }
  // A coded read with a live plain binding is bound; PS_STUB_NOT_SMALLER
  // makes it the not-smaller record instead.
  if (coded != nullptr && (out->base.flags & 0x200u) != 0) {
    const bool not_smaller = EnvOr("PS_STUB_NOT_SMALLER", 0) != 0;
    if (coding == 1) out->base.flags |= not_smaller ? 0x10000u : 0x4000u;
    if (coding == 2) out->base.flags |= not_smaller ? 0x20000u : 0x8000u;
  }
  return 0;
}

int ps_html_markdown_etag_coded(const uint8_t* binding_hash,
                                const uint8_t* markdown_hash, int coding,
                                char* buf, size_t capacity, size_t* out_len) {
  if (coding < 0 || coding > 2) return -1;
  const int rc = ps_html_markdown_etag(binding_hash, markdown_hash, buf,
                                       capacity, out_len);
  // Not the real hash: the coding's digit in place of the tag's last one.
  if (rc == 0 && coding != 0 && *out_len >= 2) {
    buf[*out_len - 2] = coding == 1 ? 'g' : 'b';
  }
  return rc;
}

int ps_read_shared_config_markdown_codings(const char* cache_path) {
  if (cache_path == nullptr) return -1;
  return static_cast<int>(EnvOr("PS_STUB_MARKDOWN_CODINGS", 3));
}
#endif  // PS_STUB_HTML_SERVE_16
#endif  // PS_STUB_HTML_SERVE_15

int ps_cache_remove_site(void* cache, const char* url, const char* hostname,
                         const char* scheme, const char* site) {
  if (cache == nullptr || url == nullptr || hostname == nullptr ||
      !StubSiteIsValid(site)) {
    return kErrInvalidArg;
  }
  const long forced = EnvOr("PS_STUB_REMOVE_STATUS", 0);
  if (forced != 0) return static_cast<int>(forced);
  SiteLog("remove", site, url);
  StubSidecar* found = FindSidecar(url, site);
  if (found == nullptr) return kErrNotFound;
  memset(found, 0, sizeof(*found));
  return kOk;
}

int ps_cache_write_site_purge_epoch(void* cache, const char* site,
                                    uint64_t epoch_s) {
  if (cache == nullptr || !StubSiteIsNamed(site)) return kErrInvalidArg;
  const long forced = EnvOr("PS_STUB_EPOCH_WRITE_STATUS", 0);
  if (forced != 0) return static_cast<int>(forced);
  if (epoch_s > static_cast<uint64_t>(time(nullptr)) + 5) {
    return kErrInvalidArg;
  }
  StubEpoch* slot = nullptr;
  for (size_t i = 0; i < kSidecarSlots; ++i) {
    if (g_epochs[i].used && strcmp(g_epochs[i].site, site) == 0) {
      slot = &g_epochs[i];
    }
  }
  for (size_t i = 0; slot == nullptr && i < kSidecarSlots; ++i) {
    if (!g_epochs[i].used) {
      slot = &g_epochs[i];
      slot->used = 1;
      snprintf(slot->site, sizeof(slot->site), "%s", site);
      slot->value = 0;
    }
  }
  if (slot == nullptr) return kErrIo;
  if (epoch_s > slot->value) slot->value = epoch_s;  // max-merge
  return kOk;
}

#ifndef PS_STUB_OMIT_SITE_PURGE_EPOCH_READ
int ps_cache_read_site_purge_epoch(void* cache, const char* site,
                                   uint64_t* out_epoch_s) {
  if (cache == nullptr || out_epoch_s == nullptr || !StubSiteIsNamed(site)) {
    return kErrInvalidArg;
  }
  const long forced = EnvOr("PS_STUB_EPOCH_READ_STATUS", 0);
  if (forced != 0) return static_cast<int>(forced);
  for (size_t i = 0; i < kSidecarSlots; ++i) {
    if (g_epochs[i].used && strcmp(g_epochs[i].site, site) == 0) {
      *out_epoch_s = g_epochs[i].value;
      return kOk;
    }
  }
  *out_epoch_s = 0;
  return kOk;
}
#endif  // PS_STUB_OMIT_SITE_PURGE_EPOCH_READ
#endif  // PS_STUB_HTML_SERVE

int ps_cache_open(const StubCacheConfig* config, void** out_cache) {
  if (out_cache == nullptr) return 5;
  static int placeholder = 0;
  *out_cache = &placeholder;
  return 0;
}

void ps_cache_close(void* cache) {}

const char* ps_strerror(int error) { return "stub error"; }

#ifndef PS_STUB_OMIT_LAST_ERROR
// The per-failure explanation.  Fixed rather than derived from the last call:
// what is under test is that the loader BINDS it and the adapter joins it to
// the error class, not the peer's own bookkeeping.
const char* ps_last_error_message() { return "stub reason"; }
#endif  // PS_STUB_OMIT_LAST_ERROR

// ---------------------------------------------------------------------------
// The record arm.
// ---------------------------------------------------------------------------

void ps_write_params_init_sized(StubWriteParams* params, size_t size) {
  // The minimum-size refusal is SILENT and leaves the struct untouched --
  // below the width of the size field there is nowhere to record the size,
  // and stamping it would be the overrun this exists to prevent.
  if (params == nullptr || size < sizeof(size_t)) {
    return;
  }
  memset(params, 0, size);
  params->struct_size = size;
}

void ps_notify_params_init_sized(StubNotifyParams* params, size_t size) {
  if (params == nullptr || size < sizeof(size_t)) {
    return;
  }
  memset(params, 0, size);
  params->struct_size = size;
}

#ifndef PS_STUB_OMIT_SITE
int ps_cache_write_original_site(void* cache, const char* url,
                                 const char* hostname, const char* scheme,
                                 const char* site,
                                 const StubWriteParams* params, void** out) {
  if (cache == nullptr || params == nullptr || out == nullptr ||
      url == nullptr || hostname == nullptr || !StubSiteIsValid(site)) {
    return kErrInvalidArg;
  }
  const size_t usable = ClampWriteParamsSize(params->struct_size);
  if (usable == 0) {
    return kErrInvalidArg;
  }
  // Read exactly the clamped prefix, as the peer does.  Anything past it is
  // NOT an error and NOT truncation the caller hears about -- it is simply
  // dropped, which is the whole reason the module static_asserts its size.
  StubWriteParams p;
  memset(&p, 0, sizeof(p));
  memcpy(&p, params, usable < sizeof(p) ? usable : sizeof(p));

  // The class has exactly one id, which is what lets "the original" mean one
  // thing.  Nothing else is writable through this entry point.
  if (p.alternate_id != 0 && p.alternate_id != kSentinelOriginal) {
    return kErrInvalidArg;
  }
  if ((p.full_mask & 0xFFu) != 0 &&
      (p.full_mask & 0xFFu) != kSentinelOriginal) {
    return kErrInvalidArg;
  }
  if (p.content_length > kOriginalContentCap) {
    return kErrNoSpace;
  }

  char line[1024];
  snprintf(
      line, sizeof(line),
      "write_original url=%s host=%s scheme=%s alternate_id=%u "
      "full_mask=%u content_type=%d flags=%u content_length=%llu "
      "params_size=%zu usable=%zu cache_inserted_at=%u max_age=%u "
      "s_maxage=%u cc_flags=%u last_modified=%u etag=%s origin_ct=%s",
      url, hostname, scheme == nullptr ? "" : scheme,
      static_cast<unsigned>(p.alternate_id), static_cast<unsigned>(p.full_mask),
      p.content_type, static_cast<unsigned>(p.flags),
      static_cast<unsigned long long>(p.content_length), params->struct_size,
      usable, static_cast<unsigned>(p.cache_inserted_at),
      static_cast<unsigned>(p.origin_max_age),
      static_cast<unsigned>(p.origin_s_maxage),
      static_cast<unsigned>(p.origin_cc_flags),
      static_cast<unsigned>(p.origin_last_modified),
      p.origin_etag == nullptr ? "" : p.origin_etag,
      p.origin_ct == nullptr ? "" : p.origin_ct);
  Log(line);
  SiteLog("write_original", site, url);

  StubWriteHandle* handle =
      static_cast<StubWriteHandle*>(calloc(1, sizeof(StubWriteHandle)));
  if (handle == nullptr) {
    return kErrInvalidArg;
  }
  handle->cap = kOriginalContentCap;
  snprintf(handle->url, sizeof(handle->url), "%s", url);
  *out = handle;
  return kOk;
}
#endif  // PS_STUB_OMIT_SITE

int ps_write_data(void* handle, const void* data, size_t length) {
  if (handle == nullptr) {
    return kErrInvalidArg;
  }
  StubWriteHandle* h = static_cast<StubWriteHandle*>(handle);
  // Sticky: once a write has failed, every later one is refused rather than
  // silently accepted onto an entry that no longer exists.
  if (h->errored) {
    return kErrClosed;
  }
  if (length > 0 && data == nullptr) {
    return kErrInvalidArg;
  }
  if (length > h->cap - h->written) {
    h->errored = true;
    h->over_cap = true;
    return kErrNoSpace;
  }
  h->written += length;
  return kOk;
}

int ps_write_close(void* handle) {
  if (handle == nullptr) {
    return kOk;
  }
  StubWriteHandle* h = static_cast<StubWriteHandle*>(handle);
  const bool over_cap = h->over_cap;
  char line[512];
  snprintf(line, sizeof(line), "write_close url=%s bytes=%llu over_cap=%d",
           h->url, static_cast<unsigned long long>(h->written),
           over_cap ? 1 : 0);
  // The handle is released on EVERY path out of the close, including the
  // failing ones.
  free(h);
  if (over_cap) {
    Log(line);
    return kErrClosed;
  }
  Log(line);
  return kOk;
}

void ps_write_abort(void* handle) {
  StubWriteHandle* h = static_cast<StubWriteHandle*>(handle);
  if (h != nullptr) {
    char line[640];
    snprintf(line, sizeof(line), "write_abort url=%s", h->url);
    Log(line);
  }
  free(h);
}

int ps_vary_uncacheable(const char* vary) {
  return ClassifyVaryForStore(vary).storable ? 0 : 1;
}

#ifndef PS_STUB_OMIT_VARIES_ACCEPT
// The 1.7 half.  Reproduced from the peer with its pairing rule intact: this
// is ALWAYS 0 for a response the refusal predicate refuses, because both are
// wrappers over ONE classification of one value.  A stand-in that answered
// them independently could agree with the module while the real library
// disagreed with it, which is the failure the whole file exists to avoid.
int ps_vary_varies_accept(const char* vary) {
  return ClassifyVaryForStore(vary).varies_accept ? 1 : 0;
}
#endif  // PS_STUB_OMIT_VARIES_ACCEPT

int ps_parse_cache_control(const char* header_value, StubCacheControl* out) {
  if (header_value == nullptr || out == nullptr) {
    return kErrInvalidArg;
  }
  if (out->struct_size < sizeof(size_t)) {
    return kErrInvalidArg;
  }
  const size_t caller = out->struct_size;
  StubCacheControl full;
  memset(&full, 0, sizeof(full));
  memcpy(&full, out, caller < sizeof(full) ? caller : sizeof(full));

  // Set on EVERY call, before any parsing, so that after the last line this
  // bit answers "did the origin send Cache-Control at all" -- which is not
  // the same question as "is max-age zero".
  full.cc_flags |= 0x0200u;  // header present

  const size_t len = strlen(header_value);
  size_t pos = 0;
  while (pos <= len) {
    size_t comma = pos;
    while (comma < len && header_value[comma] != ',') {
      ++comma;
    }
    size_t start = pos;
    size_t end = comma;
    while (start < end && header_value[start] == ' ') ++start;
    while (end > start && header_value[end - 1] == ' ') --end;
    if (end > start) {
      size_t eq = start;
      while (eq < end && header_value[eq] != '=') ++eq;
      const bool has_argument = eq < end;
      size_t name_end = eq;
      while (name_end > start && header_value[name_end - 1] == ' ') --name_end;
      const char* name = header_value + start;
      const size_t name_len = name_end - start;

      struct Row {
        const char* name;
        uint16_t flag;
      };
      static const Row kRows[] = {
          {"no-cache", 0x0001u},         {"must-revalidate", 0x0002u},
          {"proxy-revalidate", 0x0080u}, {"no-store", 0x0004u},
          {"private", 0x0008u},          {"public", 0x0010u},
          {"immutable", 0x0020u},        {"no-transform", 0x0100u},
      };
      bool matched = false;
      for (const Row& row : kRows) {
        if (AsciiEqualsIgnoreCase(name, name_len, row.name)) {
          full.cc_flags |= row.flag;
          // The bare/qualified split turns on the PRESENCE of '=', not on a
          // non-empty value: `private=""` is qualified, bare `private` is not.
          if (row.flag == 0x0008u) {
            full.cc_flags |= has_argument ? 0x0400u : 0x0800u;
          } else if (row.flag == 0x0001u) {
            full.cc_flags |= has_argument ? 0x1000u : 0x2000u;
          }
          matched = true;
          break;
        }
      }
      if (!matched) {
        const bool is_max_age =
            AsciiEqualsIgnoreCase(name, name_len, "max-age");
        const bool is_s_maxage =
            AsciiEqualsIgnoreCase(name, name_len, "s-maxage");
        if (is_max_age || is_s_maxage) {
          uint64_t value = 0;
          bool ok = has_argument;
          for (size_t i = eq + 1; ok && i < end; ++i) {
            const char c = header_value[i];
            if (c == '"' && (i == eq + 1 || i == end - 1)) {
              continue;
            }
            if (c == ' ' && i == eq + 1) {
              continue;
            }
            if (c < '0' || c > '9') {
              value = 0;
              ok = false;
              break;
            }
            value = value * 10 + static_cast<uint64_t>(c - '0');
            if (value > 0xFFFFFFFFULL) {
              value = 0xFFFFFFFFULL;
              break;
            }
          }
          // A lifetime OVERWRITES rather than accumulates: several lines are
          // one list and the last one wins.
          if (is_max_age) {
            full.max_age = static_cast<uint32_t>(value);
          } else {
            full.s_maxage = static_cast<uint32_t>(value);
            full.cc_flags |= 0x0040u;  // s-maxage present
          }
        }
      }
    }
    if (comma >= len) {
      break;
    }
    pos = comma + 1;
  }

  full.struct_size = caller < sizeof(full) ? caller : sizeof(full);
  memcpy(out, &full, full.struct_size);
  return kOk;
}

uint32_t ps_age_adjusted_insert_time(uint32_t now_seconds,
                                     uint32_t age_seconds) {
  if (age_seconds > 0 && age_seconds <= now_seconds) {
    return now_seconds - age_seconds;
  }
  // An age larger than the clock is an anomaly where subtracting would land
  // in a different epoch; failing toward the local clock keeps the entry
  // merely over-fresh rather than nonsensical.
  return now_seconds;
}

uint32_t ps_classify(const char* accept, const char* user_agent,
                     const char* save_data, const char* accept_encoding) {
  // NULL and "" mean the same thing on every argument.
  const char* a = accept == nullptr ? "" : accept;
  const char* ua = user_agent == nullptr ? "" : user_agent;
  const char* sd = save_data == nullptr ? "" : save_data;
  const char* ae = accept_encoding == nullptr ? "" : accept_encoding;

  // Bit layout, which is the part that must be exact: image format 0-1,
  // viewport 2-3, pixel density 4, save-data 5, transfer encoding 6-7.
  uint32_t image_format = 0;
  if (strstr(a, "image/avif") != nullptr) {
    image_format = 2;
  } else if (strstr(a, "image/webp") != nullptr ||
             strstr(a, "image/*") != nullptr || strstr(a, "*/*") != nullptr) {
    image_format = 1;
  }
  uint32_t viewport = 2;  // desktop
  if (strstr(ua, "iPad") != nullptr || strstr(ua, "Tablet") != nullptr) {
    viewport = 1;
  } else if (strstr(ua, "Mobile") != nullptr ||
             strstr(ua, "iPhone") != nullptr) {
    viewport = 0;
  }
  // PS_STUB_CLASSIFY_VIEWPORT: a DEFECT SHAPE, not a capability the real
  // classifier has.  The reserved viewport value 3 is the sentinel marker
  // (low byte bits 2-3 == 3), and the peer's FromHeaders() cannot produce
  // it -- which is exactly why the serve arm's by-id retry needs a way to be
  // handed one anyway: its sentinel guard exists for the day a mask ever
  // arrives carrying a value nothing produces today, and a stand-in that
  // could not play that shape could not test the guard.  Unset (the default
  // and every other test's case) the classifier is faithful: mobile, tablet,
  // desktop, and nothing else.
  {
    const char* v = StubGetenv("PS_STUB_CLASSIFY_VIEWPORT");
    if (v != nullptr && *v != '\0') {
      viewport = static_cast<uint32_t>(strtoul(v, nullptr, 10)) & 0x03u;
    }
  }
  const uint32_t density = viewport == 0 ? 1 : 0;
  const uint32_t save = (strcmp(sd, "on") == 0 || strcmp(sd, "1") == 0) ? 1 : 0;
  uint32_t encoding = 0;
  if (strstr(ae, "br") != nullptr) {
    encoding = 2;
  } else if (strstr(ae, "gzip") != nullptr) {
    encoding = 1;
  }
  return (image_format & 0x03u) | ((viewport & 0x03u) << 2) |
         ((density & 0x01u) << 4) | ((save & 0x01u) << 5) |
         ((encoding & 0x03u) << 6);
}

int ps_classify_content_type(const char* content_type_header) {
  if (content_type_header == nullptr) {
    return 4;  // other
  }
  // Truncate at the first ';' and trim SPACE only -- not HTAB, which is the
  // detail a re-implementation gets wrong: "text/html\t" is NOT html here.
  const char* begin = content_type_header;
  const char* end = strchr(begin, ';');
  if (end == nullptr) {
    end = begin + strlen(begin);
  }
  while (begin < end && *begin == ' ') ++begin;
  while (end > begin && end[-1] == ' ') --end;
  const size_t len = static_cast<size_t>(end - begin);
  if (len == 0) {
    return 4;
  }
  const char* html[] = {"text/html", "application/xhtml+xml"};
  for (const char* c : html) {
    if (AsciiEqualsIgnoreCase(begin, len, c)) return 0;
  }
  if (AsciiEqualsIgnoreCase(begin, len, "text/css")) return 1;
  const char* js[] = {"application/javascript", "text/javascript",
                      "application/x-javascript", "application/ecmascript",
                      "text/ecmascript"};
  for (const char* c : js) {
    if (AsciiEqualsIgnoreCase(begin, len, c)) return 2;
  }
  if (len >= 6 && AsciiEqualsIgnoreCase(begin, 6, "image/")) {
    return 3;
  }
  return 4;
}

int ps_option_context_signature(const char* payload, size_t payload_length,
                                char* out, size_t out_size) {
  if (payload == nullptr && payload_length != 0) {
    return kErrInvalidArg;
  }
  if (out == nullptr || out_size < kSignatureChars + 1) {
    return kErrInvalidArg;
  }
  if (payload_length > kMaxOptionContextBytes) {
    return kErrInvalidArg;
  }
  Sha256Hex(payload, payload_length, out);
  return kOk;
}

int ps_notify_worker_ex(const char* socket_path,
                        const StubNotifyParams* params) {
  if (socket_path == nullptr || params == nullptr) {
    return kErrInvalidArg;
  }
  if (params->struct_size < sizeof(size_t)) {
    return kErrInvalidArg;
  }
  // Three thresholds, not a ladder: everything up to and including the mask
  // is the core, the agent bit is next, and ALL THREE option-context fields
  // are gated on the last of them being present.
  const size_t core_end =
      offsetof(StubNotifyParams, mask) + sizeof(params->mask);
  const size_t context_end = offsetof(StubNotifyParams, option_signature) +
                             sizeof(params->option_signature);
  if (params->struct_size < core_end) {
    return kErrInvalidArg;
  }
  if (params->url == nullptr || params->hostname == nullptr) {
    return kErrInvalidArg;
  }
  if (params->content_type > 4) {
    return kErrInvalidArg;
  }
  if (params->scheme == nullptr) {
    return kErrInvalidArg;
  }
  if (strcmp(params->scheme, "http") != 0 &&
      strcmp(params->scheme, "https") != 0) {
    return kErrInvalidArg;
  }

  const bool has_context = params->struct_size >= context_end;
  const char* context = has_context ? params->option_context : nullptr;
  const size_t context_length = has_context ? params->option_context_length : 0;
  const char* signature = has_context ? params->option_signature : nullptr;

  // Both or neither.  Either half alone is refused rather than dropped,
  // because a notification that quietly lost its context would be processed
  // under the default one.
  if ((context == nullptr) != (signature == nullptr)) {
    return kErrInvalidArg;
  }
  if (context != nullptr) {
    if (context_length > kMaxOptionContextBytes) {
      return kErrInvalidArg;
    }
    // A non-null but EMPTY pair is refused too: "no context" is spelled with
    // two nulls, not with two empty strings.
    const size_t sig_length = strlen(signature);
    if (context_length == 0 && sig_length == 0) {
      return kErrInvalidArg;
    }
    if (context_length < 6 || memcmp(context, "psoc1\n", 6) != 0) {
      return kErrInvalidArg;
    }
    if (sig_length != kSignatureChars) {
      return kErrInvalidArg;
    }
    for (size_t i = 0; i < sig_length; ++i) {
      const char c = signature[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        return kErrInvalidArg;
      }
    }
    // RECOMPUTED, not trusted.  A signature that does not match its payload
    // names something the sender never had.
    char expected[kSignatureChars + 1];
    Sha256Hex(context, context_length, expected);
    if (memcmp(expected, signature, kSignatureChars) != 0) {
      return kErrInvalidArg;
    }
  }

  // 1.12: the site and the flags, both or neither by struct size.  A caller
  // whose struct predates them is unsited and unvouched.
  const int agent_request =
      params->struct_size >= offsetof(StubNotifyParams, agent_request) +
                                 sizeof(params->agent_request)
          ? params->agent_request
          : 0;
  const bool has_site_fields =
      params->struct_size >=
      offsetof(StubNotifyParams, notify_flags) + sizeof(params->notify_flags);
  const char* site = has_site_fields ? params->site : nullptr;
  const uint32_t flags = has_site_fields ? params->notify_flags : 0;
  if (!StubSiteIsValid(site)) {
    return kErrInvalidArg;
  }
  if ((flags & ~(kNotifyFlagHostVouched | kNotifyFlagFromModule)) != 0) {
    return kErrInvalidArg;
  }
  // The module's own rules, as the peer enforces them.
  if ((flags & kNotifyFlagFromModule) != 0 &&
      (site == nullptr || *site == '\0' || params->content_type == 0 ||
       agent_request != 0 || params->mask == kLlmsTxtMask ||
       (flags & kNotifyFlagHostVouched) != 0)) {
    return kErrInvalidArg;
  }

  char line[1024];
  snprintf(line, sizeof(line),
           "notify socket=%s url=%s host=%s scheme=%s content_type=%d mask=%u "
           "agent_request=%d params_size=%zu context_length=%zu signature=%s "
           "site=%s flags=%u",
           socket_path, params->url, params->hostname, params->scheme,
           params->content_type, static_cast<unsigned>(params->mask),
           agent_request, params->struct_size, context_length,
           signature == nullptr ? "" : signature, StubSiteForLog(site),
           static_cast<unsigned>(flags));
  Log(line);

  if (EnvOr("PS_STUB_NOTIFY_FAIL", 0) != 0) {
    return kErrIo;
  }
  return kOk;
}

// ---------------------------------------------------------------------------
// The serve arm.
// ---------------------------------------------------------------------------

#ifndef PS_STUB_OMIT_SITE
int ps_cache_read_best_site(void* cache, const char* url, const char* hostname,
                            const char* scheme, const char* site, uint32_t mask,
                            void** out) {
  if (cache == nullptr || url == nullptr || hostname == nullptr ||
      out == nullptr || !StubSiteIsValid(site)) {
    return kErrInvalidArg;
  }
  *out = nullptr;
  SiteLog("read_best", site, url);
  if (!StubSiteSeesEntries(site)) {
    return kErrNotFound;
  }
  StubAlternate table[16];
  const size_t count = StubParseAlternates(table, 16);
  int best_score = 0;
  const StubAlternate* best = nullptr;
  for (size_t i = 0; i < count; ++i) {
    // THE SKIP.  The durable-originals class is never selectable, by any
    // request; it is read back deliberately, by id.  A serving path built on
    // this call alone finds nothing for a URL only the module recorded.
    if (table[i].id == kSentinelOriginal) {
      continue;
    }
    if (StubIsSentinel(table[i].id)) {
      continue;
    }
    const int score = StubScoreAlternate(mask, table[i].id);
    if (score > best_score) {
      best_score = score;
      best = &table[i];
    }
  }
  if (best == nullptr) {
    return kErrNotFound;
  }
  StubReadResult* result = StubMakeResult(*best);
  if (result == nullptr) {
    return kErrInvalidArg;
  }
  char line[256];
  snprintf(line, sizeof(line), "read_best url=%s mask=%u selected=%u score=%d",
           url, static_cast<unsigned>(mask), static_cast<unsigned>(best->id),
           best_score);
  Log(line);
  *out = result;
  return kOk;
}

int ps_cache_read_alternate_site(void* cache, const char* url,
                                 const char* hostname, const char* scheme,
                                 const char* site, uint8_t alternate_id,
                                 void** out) {
  if (cache == nullptr || url == nullptr || hostname == nullptr ||
      out == nullptr || !StubSiteIsValid(site)) {
    return kErrInvalidArg;
  }
  *out = nullptr;
  SiteLog("read_alternate", site, url);
  // A steered failure, standing for the states a real volume reaches:
  // busy while another process re-records, an I/O error, a corrupted
  // entry, a closed volume.  Before any alternate is consulted, because
  // the point is that the READ failed, not that a particular id was
  // missing -- the statuses the serve arm's probe has to treat as
  // restrictions-unknown rather than as none.
  const long forced_status = EnvOr("PS_STUB_READ_ALT_STATUS", 0);
  if (forced_status != 0) {
    char line[256];
    snprintf(line, sizeof(line), "read_alternate url=%s id=%u err=%ld", url,
             static_cast<unsigned>(alternate_id), forced_status);
    Log(line);
    return static_cast<int>(forced_status);
  }
  StubAlternate table[16];
  const size_t count =
      StubSiteSeesEntries(site) ? StubParseAlternates(table, 16) : 0;
  for (size_t i = 0; i < count; ++i) {
    if (table[i].id != alternate_id) {
      continue;
    }
    StubAlternate chosen = table[i];
    if (alternate_id == 0x0C) {
      StubApplyOriginalFlagsSequence(&chosen);
    }
    StubReadResult* result = StubMakeResult(chosen);
    if (result == nullptr) {
      return kErrInvalidArg;
    }
    char line[256];
    snprintf(line, sizeof(line), "read_alternate url=%s id=%u", url,
             static_cast<unsigned>(alternate_id));
    Log(line);
    *out = result;
    return kOk;
  }
  char line[256];
  snprintf(line, sizeof(line), "read_alternate url=%s id=%u miss", url,
           static_cast<unsigned>(alternate_id));
  Log(line);
  return kErrNotFound;
}
#endif  // PS_STUB_OMIT_SITE

int ps_read_content(const void* result, const uint8_t** out_data,
                    size_t* out_length) {
  if (result == nullptr || out_data == nullptr || out_length == nullptr) {
    return kErrInvalidArg;
  }
  const StubReadResult* r = static_cast<const StubReadResult*>(result);
  *out_data = reinterpret_cast<const uint8_t*>(r->body);
  *out_length = r->body_length;
  return kOk;
}

uint32_t ps_read_mask(const void* result) {
  // The peer derives the stored mask from the alternate id: all eight
  // capability bits fit in the low byte, which IS the id.
  return result == nullptr
             ? 0u
             : static_cast<uint32_t>(
                   static_cast<const StubReadResult*>(result)->alternate.id);
}

uint8_t ps_read_flags(const void* result) {
  return result == nullptr
             ? 0
             : static_cast<const StubReadResult*>(result)->alternate.flags;
}

int ps_read_content_type(const void* result) {
  return static_cast<int>(EnvOr("PS_STUB_READ_CONTENT_TYPE", 3));
}

const char* ps_read_origin_content_type(const void* result) {
  const char* v = getenv("PS_STUB_ORIGIN_CT");
  return (v == nullptr || *v == '\0') ? nullptr : v;
}

uint32_t ps_read_cache_inserted_at(const void* result) {
  return static_cast<uint32_t>(EnvOr("PS_STUB_INSERTED_AT", 1000));
}

uint32_t ps_read_origin_max_age(const void* result) {
  return static_cast<uint32_t>(EnvOr("PS_STUB_ORIGIN_MAXAGE", 600));
}

uint32_t ps_read_origin_s_maxage(const void* result) {
  return static_cast<uint32_t>(EnvOr("PS_STUB_ORIGIN_SMAXAGE", 0));
}

uint16_t ps_read_origin_cc_flags(const void* result) {
  // Defaults to "the origin sent a Cache-Control header", which is what
  // makes origin_max_age meaningful rather than a content-type default.
  return static_cast<uint16_t>(EnvOr("PS_STUB_ORIGIN_CCFLAGS", 0x0200));
}

uint32_t ps_read_origin_last_modified(const void* result) {
  return static_cast<uint32_t>(EnvOr("PS_STUB_ORIGIN_LM", 0));
}

const char* ps_read_origin_etag(const void* result) {
  const char* v = getenv("PS_STUB_ORIGIN_ETAG");
  return (v == nullptr || *v == '\0') ? nullptr : v;
}

const uint8_t* ps_read_origin_html_hash(const void* result) {
  static uint8_t hash[32];
  const char* v = getenv("PS_STUB_ORIGIN_HASH_BYTE");
  if (v == nullptr || *v == '\0') {
    return nullptr;
  }
  memset(hash, static_cast<int>(strtol(v, nullptr, 16)), sizeof(hash));
  return hash;
}

int ps_read_is_worker_processed(const void* result) {
  return result == nullptr ? 0
                           : static_cast<const StubReadResult*>(result)
                                 ->alternate.worker_processed;
}

uint32_t ps_read_origin_content_length(const void* result) {
  return static_cast<uint32_t>(EnvOr("PS_STUB_ORIGIN_CONTENT_LENGTH", 0));
}

void ps_read_free(void* result) { free(result); }

// A MODEL of the peer's freshness evaluator, not a second implementation of
// RFC 9111: enough of it that the module's plumbing (which fields go in,
// which verdict comes out, what the arm does about each) is under test, and
// no more.  The real dialect is graded on the live rig, against the peer's
// own build.
int ps_evaluate_freshness(const StubFreshnessInput* input,
                          const StubFreshnessConfig* config,
                          StubFreshnessResult* out) {
  if (input == nullptr || out == nullptr || input->struct_size == 0 ||
      out->struct_size == 0) {
    return kErrInvalidArg;
  }
  const uint32_t age = input->now_seconds > input->cache_inserted_at
                           ? input->now_seconds - input->cache_inserted_at
                           : 0;
  uint32_t effective = input->origin_max_age;
  if ((input->origin_cc_flags & 0x0040u) != 0) {  // s-maxage present
    effective = input->origin_s_maxage;           // shared cache prefers it
  }
  // TYPE DEFAULTS FOR A SILENT ORIGIN, the peer's own step and numbers:
  // an entry whose stored state records neither a lifetime nor a
  // `Cache-Control` line (0x0200, the header-present bit) is served on the
  // default -- 300 s for a stylesheet or script, 3600 s for an image --
  // not refused.  The stand-in used to model zero as always-refused, which
  // made an `Expires`-only origin look safe to store as silence; the peer
  // serves it, which is why the record arm derives that lifetime now.
  // HTML is deliberately not modelled: the peer's html default is 0, and
  // this module's arm declines HTML before it ever evaluates freshness.
  if (effective == 0 && (input->origin_cc_flags & 0x0200u) == 0) {
    if (input->content_type == 1 || input->content_type == 2) {  // css, js
      effective = 300u;
    } else if (input->content_type == 3) {  // image
      effective = 3600u;
    }
  }
  if (effective > 86400u) {
    effective = 86400u;
  }
  out->age_seconds = age;
  out->effective_max_age = effective;
  out->remaining_ttl = age < effective ? effective - age : 0;
  // STRICTLY GREATER, matching the peer's boundary: an entry exactly at its
  // lifetime is still fresh (the peer computes is_stale as age > effective,
  // and expired_by_age with the same comparison), so the model uses the same
  // one or the boundary case would diverge from the library being stood in
  // for.
  out->is_stale = age > effective ? 1 : 0;
  out->expired_by_age = out->is_stale;
  if ((input->origin_cc_flags & 0x0001u) != 0) {  // no-cache, either form
    // THE PEER'S OWN RULE, reproduced because the module now relies on it:
    // no-cache content is permanently "stale" BY DEFINITION -- every use
    // revalidates -- and that must NOT count as expiry, or no-cache origins
    // (a very common CMS default) would churn the worker's
    // purge -> re-optimize cycle on every request.  The peer's evaluator
    // computes expired_by_age = !no_cache && (age > lifetime, strictly);
    // the bit tested here is its kCCOriginNoCache, which its parser sets
    // for the bare and the qualified form alike.  The verdict path below
    // keeps the model's existing dialect (a FRESH no-cache entry is served
    // with the directive relayed); what changes is only that a stale
    // no-cache entry no longer reports an expiry the peer would not report.
    out->expired_by_age = 0;
  }
  if (input->force_revalidate != 0) {
    // The CLIENT asked; that says nothing about whether the bytes are old.
    out->expired_by_age = 0;
    out->verdict = 3;  // REVALIDATE
  } else if (out->is_stale != 0) {
    out->verdict = 3;
  } else if ((input->origin_cc_flags & 0x2000u) != 0) {  // no-cache, bare
    out->verdict = 2;                                    // SERVE_NO_CACHE
  } else {
    out->verdict = 0;  // FRESH
  }
  return kOk;
}

// A MODEL of the peer's `Cache-Control` builder, on the same terms.  It
// reproduces the two behaviours the serve arm depends on: SAFE mode never
// synthesizes `stale-while-revalidate` and never adds `public`, and the
// origin's `no-cache` is relayed only when the caller asks for it.
int ps_build_cache_control(const StubCacheControlInput* input, char* buf,
                           size_t capacity, size_t* out_len,
                           uint32_t* out_final_max_age) {
  if (input == nullptr || buf == nullptr || out_len == nullptr ||
      capacity == 0 || input->struct_size == 0) {
    return kErrInvalidArg;
  }
  // DIRECTIVE ORDER MATCHES THE REAL LIBRARY, and it is worth a line of
  // explanation: the real builder emits `must-revalidate` BEFORE `max-age`,
  // and this stand-in emitted them the other way round. A test that pinned
  // the emitted string was then pinning the stand-in's spelling rather than
  // the emitter's, so it would have passed while the shipped behaviour
  // changed underneath it -- exactly the class of false green the whole file
  // exists to avoid.
  char scratch[256];
  int n = 0;
  if (input->mode == 0) {
    n += snprintf(scratch + n, sizeof(scratch) - n, "must-revalidate, ");
  }
  n += snprintf(scratch + n, sizeof(scratch) - n, "max-age=%u",
                static_cast<unsigned>(input->effective_max_age));
  if (input->relay_origin_no_cache != 0 &&
      (input->origin_cc_flags & 0x2000u) != 0) {
    n += snprintf(scratch + n, sizeof(scratch) - n, ", no-cache");
  }
  if (input->synthesize_swr != 0 && input->mode == 1) {
    n += snprintf(scratch + n, sizeof(scratch) - n,
                  ", stale-while-revalidate=30");
  }
  if (n <= 0) {
    return kErrInvalidArg;
  }
  size_t length = static_cast<size_t>(n);
  if (length > capacity) {
    length = capacity;
  }
  memcpy(buf, scratch, length);
  *out_len = length;
  if (out_final_max_age != nullptr) {
    *out_final_max_age = input->effective_max_age;
  }
  return kOk;
}

int ps_serve_stats_open(const char* cache_path, void** out) {
  if (cache_path == nullptr || out == nullptr) {
    return kErrInvalidArg;
  }
  *out = nullptr;
  if (EnvOr("PS_STUB_SERVE_STATS_ABSENT", 0) != 0) {
    // The worker has not created the file yet.  An ordinary state.
    return kErrNotFound;
  }
  static int placeholder = 0;
  *out = &placeholder;
  return kOk;
}

void ps_serve_stats_record_serve_class(void* handle, int serve_class,
                                       uint32_t flags) {
  if (handle == nullptr) {
    return;
  }
  char line[128];
  snprintf(line, sizeof(line), "serve_class class=%d flags=%u", serve_class,
           static_cast<unsigned>(flags));
  Log(line);
}

void ps_serve_stats_record_hit(void* handle, int content_type,
                               uint64_t original_bytes,
                               uint64_t optimized_bytes, uint32_t mask) {
  if (handle == nullptr) {
    return;
  }
  char line[160];
  snprintf(line, sizeof(line),
           "serve_hit type=%d original=%llu optimized=%llu mask=%x",
           content_type, static_cast<unsigned long long>(original_bytes),
           static_cast<unsigned long long>(optimized_bytes),
           static_cast<unsigned>(mask));
  Log(line);
}

#ifndef PS_STUB_OMIT_GENERATION
// The host-aware recorder (1.11).  Bound OPTIONALLY by the module: the
// pre-generation flavour lacks it, which is what proves an older optimizer
// still gets the hit -- without the host.
void ps_serve_stats_record_hit_host(void* handle, int content_type,
                                    uint64_t original_bytes,
                                    uint64_t optimized_bytes, uint32_t mask,
                                    const char* host, size_t host_len) {
  if (handle == nullptr) {
    return;
  }
  const int shown = static_cast<int>(host_len > 120 ? 120 : host_len);
  char line[256];
  snprintf(line, sizeof(line),
           "serve_hit_host type=%d original=%llu optimized=%llu mask=%x "
           "host=%.*s",
           content_type, static_cast<unsigned long long>(original_bytes),
           static_cast<unsigned long long>(optimized_bytes),
           static_cast<unsigned>(mask), shown, host == nullptr ? "" : host);
  Log(line);
}

// The split recorder and its reader (1.12).  Optional on the module's side:
// the pre-generation flavour lacks both, which proves an older optimizer
// still gets the hit, without the split.  PS_STUB_ENCODED_ORIGIN_LENGTH names
// the size the stub's entry reports; unset, the entry reports none.
int ps_read_encoded_origin_length(const void* result, uint64_t* out) {
  const char* value = getenv("PS_STUB_ENCODED_ORIGIN_LENGTH");
  if (value == nullptr || out == nullptr) {
    return 0;
  }
  *out = strtoull(value, nullptr, 10);
  return 1;
}

void ps_serve_stats_record_hit_split(void* handle, int content_type,
                                     uint64_t original_bytes,
                                     uint64_t encoded_origin_bytes,
                                     uint64_t optimized_bytes, uint32_t mask,
                                     const char* host, size_t host_len) {
  if (handle == nullptr) {
    return;
  }
  const int shown = static_cast<int>(host_len > 120 ? 120 : host_len);
  char line[256];
  snprintf(line, sizeof(line),
           "serve_hit_split type=%d original=%llu encoded=%llu "
           "optimized=%llu mask=%x host=%.*s",
           content_type, static_cast<unsigned long long>(original_bytes),
           static_cast<unsigned long long>(encoded_origin_bytes),
           static_cast<unsigned long long>(optimized_bytes),
           static_cast<unsigned>(mask), shown, host == nullptr ? "" : host);
  Log(line);
}
#endif  // PS_STUB_OMIT_GENERATION

void ps_serve_stats_close(void* handle) {}

}  // extern "C"
