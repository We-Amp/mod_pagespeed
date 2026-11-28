#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Assert the module's mirror of the optimizer's C API matches the optimizer.

pagespeed/system/daemon_abi.h describes the optimizer client library's C
API by hand: nothing from the optimizer's tree is included or linked, because
the library is bound at run time. So every constant and struct there is a
copy, and a copy can drift. This gate compares the copy with the optimizer's
own sources at a pinned commit:

  * every mirrored constant equals the optimizer's #define (or, for the two
    alternate ids the C header does not publish, its AlternateId enum);
  * every mirrored struct has the optimizer's fields, in the optimizer's
    order, with the same types and array lengths;
  * every entry point the module binds as the HTML serve set is declared
    (its function-pointer type in daemon_abi.cc) with the optimizer's
    prototype: the same return type and the same argument types, in order;
  * every entry point the module binds as the HTML serve set is published at
    or below the minor the module requires for that set (symbols.lds), so the
    set cannot quietly grow a symbol a 1.14 library lacks.

Usage:

    tools/ci/check_daemon_abi_mirrors.py <daemon_abi.h> <daemon_abi.cc> \\
        <optimizer pagespeed.h> <optimizer alternate_id.h> \\
        <optimizer symbols.lds>
    tools/ci/check_daemon_abi_mirrors.py --self-test
"""

import os
import re
import sys
import tempfile

# Module constant -> optimizer #define in lib/pagespeed/pagespeed.h.
DEFINES = {
    "kPsSentinelOriginal": "PS_SENTINEL_ORIGINAL",
    "kPsSentinelHeadersSidecar": "PS_SENTINEL_HEADERS_SIDECAR",
    "kPsSentinelContentHash": "PS_SENTINEL_CONTENT_HASH",
    "kPsSentinelMarkdownWithheld": "PS_SENTINEL_MARKDOWN_WITHHELD",
    "kPsFlagOriginVariesAccept": "PS_FLAG_ORIGIN_VARIES_ACCEPT",
    "kPsFlagOriginHeadersNotReproducible":
        "PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE",
    "kPsFlagHeadersClassified": "PS_FLAG_HEADERS_CLASSIFIED",
    "kPsFlagHeadersSidecarRequired": "PS_FLAG_HEADERS_SIDECAR_REQUIRED",
    "kPsSiteIdChars": "PS_SITE_ID_CHARS",
    "kPsNotifyFlagHostVouched": "PS_NOTIFY_FLAG_HOST_VOUCHED",
    "kPsNotifyFlagFromModule": "PS_NOTIFY_FLAG_FROM_MODULE",
    "kPsNotifyFlagHtmlStored": "PS_NOTIFY_FLAG_HTML_STORED",
    "kPsCcOriginNoCache": "PS_CC_ORIGIN_NO_CACHE",
    "kPsCcOriginMustRevalidate": "PS_CC_ORIGIN_MUST_REVALIDATE",
    "kPsCcOriginNoStore": "PS_CC_ORIGIN_NO_STORE",
    "kPsCcOriginPrivate": "PS_CC_ORIGIN_PRIVATE",
    "kPsCcOriginPublic": "PS_CC_ORIGIN_PUBLIC",
    "kPsCcOriginImmutable": "PS_CC_ORIGIN_IMMUTABLE",
    "kPsCcOriginSMaxagePresent": "PS_CC_ORIGIN_S_MAXAGE_PRESENT",
    "kPsCcOriginProxyRevalidate": "PS_CC_ORIGIN_PROXY_REVALIDATE",
    "kPsCcOriginNoTransform": "PS_CC_ORIGIN_NO_TRANSFORM",
    "kPsCcOriginHeaderPresent": "PS_CC_ORIGIN_HEADER_PRESENT",
    "kPsCcOriginPrivateQualified": "PS_CC_ORIGIN_PRIVATE_QUALIFIED",
    "kPsCcOriginPrivateBare": "PS_CC_ORIGIN_PRIVATE_BARE",
    "kPsCcOriginNoCacheQualified": "PS_CC_ORIGIN_NO_CACHE_QUALIFIED",
    "kPsCcOriginNoCacheBare": "PS_CC_ORIGIN_NO_CACHE_BARE",
    "kPsCcOriginSMaxageRepeated": "PS_CC_ORIGIN_S_MAXAGE_REPEATED",
    "kPsSidecarUnpaired": "PS_SIDECAR_UNPAIRED",
    "kPsSidecarPaired": "PS_SIDECAR_PAIRED",
    "kPsHtmlStoreActionRefuse": "PS_HTML_STORE_ACTION_REFUSE",
    "kPsHtmlStoreActionStore": "PS_HTML_STORE_ACTION_STORE",
    "kPsHtmlStoreActionStoreNoNotify": "PS_HTML_STORE_ACTION_STORE_NO_NOTIFY",
    "kPsHtmlStoreActionWouldStore": "PS_HTML_STORE_ACTION_WOULD_STORE",
    "kPsHtmlStoreActionWouldStoreNoNotify":
        "PS_HTML_STORE_ACTION_WOULD_STORE_NO_NOTIFY",
    "kPsHtmlServeActionFallThrough": "PS_HTML_SERVE_ACTION_FALL_THROUGH",
    "kPsHtmlServeActionServeHtml": "PS_HTML_SERVE_ACTION_SERVE_HTML",
    "kPsHtmlServeActionServeMarkdown": "PS_HTML_SERVE_ACTION_SERVE_MARKDOWN",
    "kPsHtmlServeActionValidate": "PS_HTML_SERVE_ACTION_VALIDATE",
    "kPsHtmlServeRequestHasAuthorization":
        "PS_HTML_SERVE_REQUEST_HAS_AUTHORIZATION",
    "kPsHtmlServeRequestAuthenticatedUser":
        "PS_HTML_SERVE_REQUEST_AUTHENTICATED_USER",
    "kPsHtmlServeRequestHasCookie": "PS_HTML_SERVE_REQUEST_HAS_COOKIE",
    "kPsHtmlServeRequestHasRange": "PS_HTML_SERVE_REQUEST_HAS_RANGE",
    "kPsHtmlServeRequestHasIfRange": "PS_HTML_SERVE_REQUEST_HAS_IF_RANGE",
    "kPsHtmlServeRequestHasOrigin": "PS_HTML_SERVE_REQUEST_HAS_ORIGIN",
    "kPsHtmlFactsPresent": "PS_HTML_FACTS_PRESENT",
    "kPsHtmlFactsNeedsValidation": "PS_HTML_FACTS_NEEDS_VALIDATION",
    "kPsHtmlFactsExplicitShared": "PS_HTML_FACTS_EXPLICIT_SHARED",
    "kPsHtmlFactsHeadersNotReproducible":
        "PS_HTML_FACTS_HEADERS_NOT_REPRODUCIBLE",
    "kPsHtmlFactsSidecarRequired": "PS_HTML_FACTS_SIDECAR_REQUIRED",
    "kPsHtmlFactsSidecarPresent": "PS_HTML_FACTS_SIDECAR_PRESENT",
    "kPsHtmlFactsPurged": "PS_HTML_FACTS_PURGED",
    "kPsHtmlFactsValidatedThisRequest": "PS_HTML_FACTS_VALIDATED_THIS_REQUEST",
    "kPsHtmlFactsVariantPresent": "PS_HTML_FACTS_VARIANT_PRESENT",
    "kPsHtmlFactsBindingLive": "PS_HTML_FACTS_BINDING_LIVE",
    "kPsHtmlFactsNoTransform": "PS_HTML_FACTS_NO_TRANSFORM",
    "kPsHtmlFactsWithheld": "PS_HTML_FACTS_WITHHELD",
    "kPsHtmlWithheldBelowFloor": "PS_HTML_WITHHELD_BELOW_FLOOR",
    "kPsHtmlWithheldTooLarge": "PS_HTML_WITHHELD_TOO_LARGE",
    "kPsHtmlWithheldEncoded": "PS_HTML_WITHHELD_ENCODED",
    "kPsHtmlWithheldHashMismatch": "PS_HTML_WITHHELD_HASH_MISMATCH",
    "kPsHtmlWithheldExtractFailed": "PS_HTML_WITHHELD_EXTRACT_FAILED",
    "kPsHtmlHashBytes": "PS_HTML_HASH_BYTES",
    "kPsHtmlMarkdownEtagChars": "PS_HTML_MARKDOWN_ETAG_CHARS",
    "kPsHtmlModeOff": "PS_HTML_MODE_OFF",
    "kPsHtmlModeMarkdownStatic": "PS_HTML_MODE_MARKDOWN_STATIC",
    "kPsHtmlModeMarkdownRendered": "PS_HTML_MODE_MARKDOWN_RENDERED",
    "kPsHtmlModeFull": "PS_HTML_MODE_FULL",
    # C API 1.15.
    "kPsFlagAnonymousRecorded": "PS_FLAG_ANONYMOUS_RECORDED",
    "kPsFlagHtmlStoreV2": "PS_FLAG_HTML_STORE_V2",
    "kPsHtmlStoreRequestExplicitPublic":
        "PS_HTML_STORE_REQUEST_EXPLICIT_PUBLIC",
    "kPsHtmlStoreFlagRequestAnonymous": "PS_HTML_STORE_FLAG_REQUEST_ANONYMOUS",
    "kPsHtmlServeRequestFreshOnly": "PS_HTML_SERVE_REQUEST_FRESH_ONLY",
    "kPsHtmlServeRequestHtmlFallback": "PS_HTML_SERVE_REQUEST_HTML_FALLBACK",
    "kPsHtmlServeRequestConditional": "PS_HTML_SERVE_REQUEST_CONDITIONAL",
    "kPsHtmlServeRequestExplicitPublic":
        "PS_HTML_SERVE_REQUEST_EXPLICIT_PUBLIC",
    "kPsHtmlServeScopeAgents": "PS_HTML_SERVE_SCOPE_AGENTS",
    "kPsHtmlServeScopeAll": "PS_HTML_SERVE_SCOPE_ALL",
    "kPsHtmlAgentClassMax": "PS_HTML_AGENT_CLASS_MAX",
    "kPsHtmlFactsStoreV2": "PS_HTML_FACTS_STORE_V2",
    "kPsHtmlFactsAnonymousRecorded": "PS_HTML_FACTS_ANONYMOUS_RECORDED",
    "kPsSidecarWritePolicyBlock": "PS_SIDECAR_WRITE_POLICY_BLOCK",
    "kPsHtmlGuardNone": "PS_HTML_GUARD_NONE",
    "kPsHtmlGuardRequestHeader": "PS_HTML_GUARD_REQUEST_HEADER",
    "kPsHtmlGuardPath": "PS_HTML_GUARD_PATH",
    "kPsHtmlCookiesNone": "PS_HTML_COOKIES_NONE",
    "kPsHtmlCookiesIgnorableOnly": "PS_HTML_COOKIES_IGNORABLE_ONLY",
    "kPsHtmlCookiesBlocking": "PS_HTML_COOKIES_BLOCKING",
    "kPsHtmlCookieListVersion": "PS_HTML_COOKIE_LIST_VERSION",
    "kPsHtmlCredentialedAnonymousOnly": "PS_HTML_CREDENTIALED_ANONYMOUS_ONLY",
    "kPsHtmlCredentialedExplicitPublic":
        "PS_HTML_CREDENTIALED_EXPLICIT_PUBLIC",
    # C API 1.16.
    "kPsSentinelMarkdownBr": "PS_SENTINEL_MARKDOWN_BR",
    "kPsSentinelMarkdownGzip": "PS_SENTINEL_MARKDOWN_GZIP",
    "kPsNotifyFlagMarkdownCompress": "PS_NOTIFY_FLAG_MARKDOWN_COMPRESS",
    "kPsHtmlServeRequestIdentityOk": "PS_HTML_SERVE_REQUEST_IDENTITY_OK",
    "kPsHtmlServeRequestEncodingNone": "PS_HTML_SERVE_REQUEST_ENCODING_NONE",
    "kPsMarkdownEncodingIdentity": "PS_MARKDOWN_ENCODING_IDENTITY",
    "kPsMarkdownEncodingGzip": "PS_MARKDOWN_ENCODING_GZIP",
    "kPsMarkdownEncodingBr": "PS_MARKDOWN_ENCODING_BR",
    "kPsHtmlFactsGzipCopyBound": "PS_HTML_FACTS_GZIP_COPY_BOUND",
    "kPsHtmlFactsBrCopyBound": "PS_HTML_FACTS_BR_COPY_BOUND",
    "kPsHtmlFactsGzipNotSmaller": "PS_HTML_FACTS_GZIP_NOT_SMALLER",
    "kPsHtmlFactsBrNotSmaller": "PS_HTML_FACTS_BR_NOT_SMALLER",
}

# Module constant -> optimizer enumerator in the ps_html_cache_mode_t enum.
ENUMERATORS = {
    "kPsHtmlCacheOff": "PS_HTML_CACHE_OFF",
    "kPsHtmlCacheReportOnly": "PS_HTML_CACHE_REPORT_ONLY",
    "kPsHtmlCacheServeVariants": "PS_HTML_CACHE_SERVE_VARIANTS",
    "kPsHtmlCacheServeCachedHtml": "PS_HTML_CACHE_SERVE_CACHED_HTML",
}

# Module constant -> AlternateId enumerator in lib/classify/alternate_id.h
# (ids the C header does not publish as a #define).
ALTERNATE_IDS = {
    "kPsSentinelAgentMarkdown": "kAgentMarkdown",
}

# Module struct -> optimizer typedef.
STRUCTS = {
    "PsWriteParams": "ps_write_params_t",
    "PsNotifyParams": "ps_notify_params_t",
    "PsSidecarWriteParams": "ps_sidecar_write_params_t",
    "PsHtmlServeRequest": "ps_html_serve_request_t",
    "PsHtmlStoredFacts": "ps_html_stored_facts_t",
    "PsHtmlServeVerdict": "ps_html_serve_verdict_t",
    "PsHtmlRequestGuard": "ps_html_request_guard_t",
    "PsHtmlCookieSpan": "ps_html_cookie_span_t",
    "PsHtmlCookieClass": "ps_html_cookie_class_t",
}

# The module's bound sets: the function that binds each (daemon_abi.cc), the
# struct of resolved pointers it fills, and the constant naming the minor
# that must publish every symbol it binds.
SETS = (
    ("BindHtmlServeSet", "HtmlServeSetFns", "kHtmlServeSetMinor"),
    ("BindHtmlServe15Set", "HtmlServe15SetFns", "kHtmlServe15SetMinor"),
    ("BindHtmlServe16Set", "HtmlServe16SetFns", "kHtmlServe16SetMinor"),
)

# Array lengths spelled with a constant on either side.
LENGTHS = {
    "kPsHtmlHashBytes": "32",
    "PS_HTML_HASH_BYTES": "32",
    "kPsHtmlMarkdownEtagChars + 8": "32",
    "PS_HTML_MARKDOWN_ETAG_CHARS + 8": "32",
}


# Peer type spellings that are the same C type as the module's. The peer's
# enums are pinned to int width; its opaque handles are void* here.
TYPE_ALIASES = {
    "ps_error_t": "int",
    "ps_content_type_t": "int",
    "ps_html_cache_mode_t": "int",
    "ps_cache_t": "void",
    "ps_read_result_t": "void",
    "ps_header_block_t": "void",
    "ps_write_handle_t": "void",
}


class DriftError(Exception):
    pass


def _norm_type(ctype):
    """One canonical spelling of a C type: aliases applied, mirror struct
    names mapped to the peer's, pointer stars attached."""
    names = dict(TYPE_ALIASES)
    for mine, peer in STRUCTS.items():
        names[mine] = peer
    words = re.findall(r"\w+|\*", ctype)
    words = [w for w in words if w not in ("PS_EXPORT", "PS_NODISCARD",
                                           "struct")]
    out = []
    for w in words:
        out.append("*" if w == "*" else names.get(w, w))
    text = " ".join(out)
    return re.sub(r"\s*\*", "*", text).strip()


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _int(value):
    return int(value.rstrip("uUlL"), 0)


def module_constants(text):
    out = {}
    for m in re.finditer(
            r"inline\s+constexpr\s+[\w:]+\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?\s*;",
            text):
        out[m.group(1)] = _int(m.group(2))
    return out


def peer_defines(text):
    out = {}
    for m in re.finditer(r"^\s*#define\s+(PS_\w+)\s+(0x[0-9A-Fa-f]+u?|\d+u?)\b",
                         text, re.M):
        out[m.group(1)] = _int(m.group(2))
    return out


def peer_enumerators(text):
    out = {}
    for m in re.finditer(r"\b(PS_HTML_CACHE_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)",
                         _strip_comments(text)):
        out[m.group(1)] = _int(m.group(2))
    return out


def peer_alternate_ids(text):
    out = {}
    for m in re.finditer(r"\b(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,",
                         _strip_comments(text)):
        out[m.group(1)] = _int(m.group(2))
    return out


def _fields(body):
    fields = []
    # An anonymous union (the peer's 1.16 request names one word twice) is
    # one field: its members, in order, as one entry.
    unions = []

    def _union(m):
        unions.append(tuple(_fields(m.group(1))))
        return " __union%d ;" % (len(unions) - 1)

    body = re.sub(r"\bunion\s*\{([^{}]*)\}\s*;", _union, body)
    for decl in body.split(";"):
        u = re.match(r"\s*__union(\d+)\s*$", decl)
        if u:
            fields.append(("union", unions[int(u.group(1))], None))
            continue
        decl = " ".join(decl.split())
        if not decl:
            continue
        m = re.match(r"(.*?)(\w+)\s*(?:\[([^\]]+)\])?$", decl)
        if not m:
            raise DriftError("cannot parse the field %r" % decl)
        ctype, name, length = m.group(1), m.group(2), m.group(3)
        length = LENGTHS.get(length.strip(), length.strip()) if length else None
        fields.append((name.strip("_"), _norm_type(ctype), length))
    return fields


def module_struct(text, name):
    text = _strip_comments(text)
    m = re.search(r"\bstruct\s+%s\s*\{" % name, text)
    if not m:
        raise DriftError("could not read struct %s from daemon_abi.h" % name)
    # To the brace that closes this one: a nested union has its own.
    depth, i = 1, m.end()
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    if depth:
        raise DriftError("could not read struct %s from daemon_abi.h" % name)
    return _fields(text[m.end():i - 1])


def peer_struct(text, name):
    text = _strip_comments(text)
    end = re.search(r"\}\s*%s\s*;" % name, text)
    start = text.rfind("typedef struct", 0, end.start()) if end else -1
    if start < 0:
        raise DriftError(
            "could not find typedef %s in the optimizer's pagespeed.h; the "
            "optimizer moved it somewhere this gate cannot read. Fix the gate "
            "rather than skipping it." % name)
    body = text[text.index("{", start) + 1:end.start()]
    return _fields(body)


def _binder_body(text, binder):
    m = re.search(r"void\s+%s\(\)\s*\{(.*?)\n  \}" % binder, text, re.S)
    if not m:
        raise DriftError("could not find %s in daemon_abi.cc" % binder)
    return m.group(1)


def module_serve_set(abi_cc_text, binder="BindHtmlServeSet"):
    symbols = re.findall(r'Bind\(\s*"(ps_\w+)"',
                         _binder_body(abi_cc_text, binder))
    if not symbols:
        raise DriftError("%s binds no symbol" % binder)
    return symbols


def _params(arglist):
    arglist = arglist.strip()
    if arglist in ("", "void"):
        return []
    out = []
    for arg in arglist.split(","):
        arg = " ".join(arg.split())
        # Drop a trailing parameter name (the module's typedefs have none).
        m = re.match(r"(.*?[\w*])\s+(\w+)$", arg)
        if m and m.group(2) not in ("int", "char", "void", "size_t",
                                    "uint8_t", "uint16_t", "uint32_t",
                                    "uint64_t", "int64_t", "const"):
            arg = m.group(1)
        out.append(_norm_type(arg))
    return out


def module_prototypes(abi_cc_text, sets=None):
    """symbol -> (return type, [argument types]) for every bound set, from
    the Bind() calls in each binder, the members of its struct of pointers
    and the *Fn aliases."""
    text = _strip_comments(abi_cc_text)
    aliases = {}
    for m in re.finditer(r"using\s+(\w+Fn)\s*=\s*(.*?)\(\s*\*\s*\)\s*\((.*?)\)\s*;",
                         text, re.S):
        aliases[m.group(1)] = (_norm_type(m.group(2)), _params(m.group(3)))
    out = {}
    for binder, fns, _ in (SETS if sets is None else sets):
        m = re.search(r"struct\s+%s\s*\{(.*?)\};" % fns, text, re.S)
        if not m:
            raise DriftError("could not find %s in daemon_abi.cc" % fns)
        member_types = dict((name, fn) for fn, name in
                            re.findall(r"(\w+Fn)\s+(\w+)\s*=", m.group(1)))
        for sym, member in re.findall(r'Bind\(\s*"(ps_\w+)",\s*&fns\.(\w+)',
                                      _binder_body(text, binder)):
            fn = member_types.get(member)
            if fn is None or fn not in aliases:
                raise DriftError("cannot resolve the type bound for %s" % sym)
            out[sym] = aliases[fn]
    return out


def peer_prototypes(peer_h_text):
    text = _strip_comments(peer_h_text)
    out = {}
    for m in re.finditer(r"PS_EXPORT\s+([^;{}()]*?)\b(ps_\w+)\s*\(([^;{}]*?)\)\s*;",
                         text, re.S):
        out[m.group(2)] = (_norm_type(m.group(1)), _params(m.group(3)))
    return out


def peer_symbol_minors(symbols_text):
    out = {}
    for m in re.finditer(r"PAGESPEED_1\.(\d+)\s*\{(.*?)\}", symbols_text,
                         re.S):
        minor = int(m.group(1))
        body = _strip_comments(m.group(2))
        for sym in re.findall(r"\b(ps_\w+)\s*;", body):
            out.setdefault(sym, minor)
    return out


def check(abi_h, abi_cc, peer_h, alternate_id_h, symbols_lds):
    ours = module_constants(abi_h)
    problems = []
    checked = 0
    for tables, theirs, what in (
            (DEFINES, peer_defines(peer_h), "pagespeed.h"),
            (ENUMERATORS, peer_enumerators(peer_h), "pagespeed.h"),
            (ALTERNATE_IDS, peer_alternate_ids(alternate_id_h),
             "alternate_id.h")):
        for mine, peer in sorted(tables.items()):
            if mine not in ours:
                problems.append("daemon_abi.h has no constant %s" % mine)
            elif peer not in theirs:
                problems.append("the optimizer's %s has no %s" % (what, peer))
            elif ours[mine] != theirs[peer]:
                problems.append("%s is %#x but the optimizer's %s is %#x" %
                                (mine, ours[mine], peer, theirs[peer]))
            checked += 1
    for mine, peer in sorted(STRUCTS.items()):
        a, b = module_struct(abi_h, mine), peer_struct(peer_h, peer)
        if a != b:
            problems.append("struct %s does not match the optimizer's %s:\n"
                            "    module:    %s\n    optimizer: %s" %
                            (mine, peer, a, b))
        checked += 1
    stripped_cc = _strip_comments(abi_cc)
    sets = []
    unbound = []
    for s in SETS:
        if re.search(r"void\s+%s\(\)" % s[0], stripped_cc):
            sets.append(s)
        elif s[2] in ours:
            # A set the header declares must be read, or a renamed binder
            # would drop its prototypes and minors from the gate silently.
            # Every such set is named, not only the first.
            unbound.append("daemon_abi.h has %s but daemon_abi.cc has no %s" %
                           (s[2], s[0]))
    if unbound:
        raise DriftError("\n".join(unbound))
    if not sets or sets[0][0] != "BindHtmlServeSet":
        raise DriftError("could not find BindHtmlServeSet in daemon_abi.cc")
    theirs_protos = peer_prototypes(peer_h)
    for sym, proto in sorted(module_prototypes(abi_cc, sets).items()):
        if sym not in theirs_protos:
            problems.append("the optimizer's pagespeed.h declares no %s" % sym)
        elif proto != theirs_protos[sym]:
            problems.append("%s is bound as %s(%s) but the optimizer declares "
                            "%s(%s)" % (sym, proto[0], ", ".join(proto[1]),
                                        theirs_protos[sym][0],
                                        ", ".join(theirs_protos[sym][1])))
        checked += 1
    minors = peer_symbol_minors(symbols_lds)
    for binder, _, minor_name in sets:
        required = ours.get(minor_name)
        if required is None:
            problems.append("daemon_abi.h has no %s" % minor_name)
            continue
        for sym in module_serve_set(abi_cc, binder):
            if sym not in minors:
                problems.append("the optimizer does not export %s" % sym)
            elif minors[sym] > required:
                problems.append(
                    "%s is published at 1.%d, after the 1.%d the module "
                    "requires for the set %s binds" %
                    (sym, minors[sym], required, binder))
            checked += 1
    if problems:
        raise DriftError("\n".join(problems))
    return checked


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


SELF_ABI_H = """
inline constexpr uint8_t kPsSentinelAgentMarkdown = 0x7C;
inline constexpr uint32_t kPsNotifyFlagHtmlStored = 0x8u;
inline constexpr int kPsHtmlCacheServeVariants = 2;
inline constexpr int kHtmlServeSetMinor = 14;
inline constexpr int kHtmlServe15SetMinor = 15;
inline constexpr size_t kPsHtmlHashBytes = 32;
struct PsHtmlStoredFacts {
  size_t struct_size;
  uint32_t flags;  // comment
  union {
    uint32_t coding;
    uint32_t spare;
  };
  uint8_t binding_hash[kPsHtmlHashBytes];
};
"""
SELF_ABI_CC = """
using HtmlServeClassifyFn = int (*)(const PsHtmlStoredFacts*, uint32_t);
using WantsAgentMarkdownFn = int (*)(const char*);
struct HtmlServeSetFns {
  HtmlServeClassifyFn serve_classify = nullptr;
  WantsAgentMarkdownFn wants_agent_markdown = nullptr;
};
  void BindHtmlServeSet() {
    Bind("ps_html_serve_classify", &fns.serve_classify, &e) &&
    Bind("ps_wants_agent_markdown", &fns.wants_agent_markdown, &e);
  }
using HtmlUnsafeFn = int (*)(const char*, int);
struct HtmlServe15SetFns {
  HtmlUnsafeFn unsafe = nullptr;
};
  void BindHtmlServe15Set() {
    Bind("ps_html_unsafe_method_invalidates", &fns.unsafe, &e);
  }
"""
SELF_PEER_H = """
#define PS_NOTIFY_FLAG_HTML_STORED 0x8u
#define PS_HTML_HASH_BYTES 32
typedef enum ps_html_cache_mode_t {
  PS_HTML_CACHE_OFF = 0,
  PS_HTML_CACHE_SERVE_VARIANTS = 2, /* 1.14 */
} ps_html_cache_mode_t;
typedef struct {
  size_t struct_size;
  uint32_t flags; /* PS_HTML_FACTS_* */
  union {
    uint32_t coding;
    uint32_t spare;
  };
  uint8_t binding_hash[PS_HTML_HASH_BYTES];
} ps_html_stored_facts_t;
PS_EXPORT int ps_html_serve_classify(const ps_html_stored_facts_t* facts,
                                     uint32_t now);
PS_EXPORT int ps_wants_agent_markdown(const char* accept);
PS_EXPORT int ps_html_unsafe_method_invalidates(const char* method, int status);
"""
SELF_ALT = "enum class AlternateId : uint8_t {\n  kAgentMarkdown = 0x7C,  // md\n};\n"
SELF_LDS = """PAGESPEED_1.2 { global: ps_wants_agent_markdown; };
PAGESPEED_1.14 { global: ps_html_serve_classify; } PAGESPEED_1.13;
PAGESPEED_1.15 { global: ps_html_unsafe_method_invalidates; } PAGESPEED_1.14;
"""


def self_test():
    global DEFINES, ENUMERATORS, ALTERNATE_IDS, STRUCTS
    saved = (DEFINES, ENUMERATORS, ALTERNATE_IDS, STRUCTS)
    DEFINES = {"kPsNotifyFlagHtmlStored": "PS_NOTIFY_FLAG_HTML_STORED",
               "kPsHtmlHashBytes": "PS_HTML_HASH_BYTES"}
    ENUMERATORS = {"kPsHtmlCacheServeVariants": "PS_HTML_CACHE_SERVE_VARIANTS"}
    ALTERNATE_IDS = {"kPsSentinelAgentMarkdown": "kAgentMarkdown"}
    STRUCTS = {"PsHtmlStoredFacts": "ps_html_stored_facts_t"}
    cases = 0
    try:
        args = [SELF_ABI_H, SELF_ABI_CC, SELF_PEER_H, SELF_ALT, SELF_LDS]
        assert check(*args) == 11
        cases += 1
        drifts = [
            (0, "0x8u;", "0x10u;"),                        # a value moved
            (0, "kPsHtmlCacheServeVariants = 2",
             "kPsHtmlCacheServeVariants = 3"),             # an enumerator
            (3, "0x7C", "0x8C"),                           # an alternate id
            (2, "  uint32_t flags; /* PS_HTML_FACTS_* */\n",
             "  uint32_t flags;\n  uint32_t stored_at;\n"),  # a field added
            (0, "  size_t struct_size;\n  uint32_t flags;",
             "  uint32_t flags;\n  size_t struct_size;"),  # fields reordered
            (2, "[PS_HTML_HASH_BYTES]", "[16]"),           # an array length
            (4, "PAGESPEED_1.14 { global: ps_html_serve_classify;",
             "PAGESPEED_1.15 { global: ps_html_serve_classify;"),  # too new
            (4, "ps_html_serve_classify;", "ps_other;"),   # not exported
            (2, "#define PS_HTML_HASH_BYTES 32\n", ""),    # peer lost a macro
            (2, "ps_html_stored_facts_t;", "ps_facts_t;"),  # peer renamed
            (1, "BindHtmlServeSet", "BindSomething"),      # gate cannot read
            (2, "  uint32_t flags; /* PS_HTML_FACTS_* */",
             "  uint64_t flags; /* PS_HTML_FACTS_* */"),   # a field widened
            (2, "uint32_t now);", "uint32_t now, int extra);"),  # arg added
            (2, "PS_EXPORT int ps_wants_agent_markdown",
             "PS_EXPORT long ps_wants_agent_markdown"),    # return changed
            (1, "int (*)(const PsHtmlStoredFacts*, uint32_t)",
             "int (*)(const PsHtmlStoredFacts*, uint64_t)"),  # module drifts
            (2, "int ps_html_unsafe_method_invalidates(const char* method, int",
             "int ps_html_unsafe_method_invalidates(const char* method, long"),
            (4, "PAGESPEED_1.15 { global: ps_html_unsafe_method_invalidates;",
             "PAGESPEED_1.16 { global: ps_html_unsafe_method_invalidates;"),
            (0, "kHtmlServe15SetMinor = 15", "kHtmlServe16SetMinor = 15"),
            (1, "struct HtmlServe15SetFns", "struct HtmlServe15Set"),
            (1, "void BindHtmlServe15Set()", "void BindHtmlServeNext()"),
            (2, "    uint32_t coding;\n    uint32_t spare;",
             "    uint32_t spare;\n    uint32_t coding;"),  # union reordered
            (0, "    uint32_t spare;\n  };", "  };\n    uint32_t spare;"),
        ]
        # Every set with no binder is named in one error, not only the first.
        both = list(args)
        both[1] = both[1].replace("void BindHtmlServeSet()",
                                  "void BindOther()").replace(
                                      "void BindHtmlServe15Set()",
                                      "void BindOther15()")
        try:
            check(*both)
        except DriftError as e:
            assert "BindHtmlServeSet" in str(e) and \
                "BindHtmlServe15Set" in str(e), str(e)
            cases += 1
        else:
            raise AssertionError("two missing binders not caught")
        for index, old, new in drifts:
            mutated = list(args)
            assert old in mutated[index], old
            mutated[index] = mutated[index].replace(old, new)
            try:
                check(*mutated)
            except DriftError:
                cases += 1
            else:
                raise AssertionError("drift not caught: %r -> %r" % (old, new))
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for i, text in enumerate(args):
                path = os.path.join(d, "f%d" % i)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
                paths.append(path)
            assert main(paths) == 0
            cases += 1
    finally:
        DEFINES, ENUMERATORS, ALTERNATE_IDS, STRUCTS = saved
    print("check_daemon_abi_mirrors self-test: %d cases passed" % cases)
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        checked = check(*[_read(p) for p in argv])
    except DriftError as e:
        for line in str(e).splitlines():
            print("::error::%s" % line)
        return 1
    print("daemon_abi.h matches the optimizer's C API: %d mirrors checked" %
          checked)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
