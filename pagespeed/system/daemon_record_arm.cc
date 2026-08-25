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

#include "pagespeed/system/daemon_record_arm.h"

#include <cstdint>
#include <cstring>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

// The marker this module's own rewriting puts in a URL it produced.
//
// This literal has SEVERAL COPIES in the tree and they must agree. The others:
// the sibling implementation of the same never-record rule on the IIS seam
// (pagespeed/iis/iis_inplace_resource_handler.cc, kPageSpeedMarker), the URL
// escaper (pagespeed/kernel/util/url_escaper.cc), and the harness's executable
// forms (tools/parity/adapter_asserts/driver.py's fixture name,
// tools/parity/daemon-probe/peer_sim.cc's IsPageSpeedShapedUrl,
// tools/parity/transition_load/harvest_log_mix.py). There is no shared home
// for all of them: the harness peers are built against the OTHER product's
// tree and cannot include anything from here. Changing the marker means
// changing every copy.
const char kRewrittenUrlMarker[] = ".pagespeed.";

// Returns `piece` as a NUL-terminated C string in `storage`, or nullptr when
// the piece is empty.
//
// EMPTY MUST BECOME nullptr, not "".  The peer's classifier treats the two the
// same, but its notification validator does not: an options context supplied
// as a non-null empty string is refused outright, whereas a null one means
// "this caller has none".  Routing every optional string through one helper
// keeps that distinction from having to be remembered at each call site.
const char* CStrOrNull(StringPiece piece, GoogleString* storage) {
  if (piece.empty()) {
    return nullptr;
  }
  piece.CopyToString(storage);
  return storage->c_str();
}

// Clamps a millisecond wall-clock stamp to the seconds field the entry holds.
// A negative input is "absent" and stays 0.
uint32_t SecondsFromMs(int64 ms) {
  if (ms <= 0) {
    return 0;
  }
  const int64 seconds = ms / 1000;
  if (seconds > static_cast<int64>(UINT32_MAX)) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(seconds);
}

uint32_t ClampToUint32(int64 value) {
  if (value <= 0) {
    return 0;
  }
  if (value > static_cast<int64>(UINT32_MAX)) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(value);
}

// ---------------------------------------------------------------------------
// The reproducible set.  See OriginHeadersAreReproducible in the header for
// where each member comes from and why the set is narrower than the peer's.
//
// Three CLOSED lists of exact field names, lower-cased here and compared
// case-insensitively, because HTTP field names are case-insensitive and the
// origin picks the spelling.
// ---------------------------------------------------------------------------

// D6.4 rule 6's allowlist, verbatim.  `Vary` is not here: it is allowlisted
// conditionally, on its tokens, below.
const char* const kReproducibleHeaders[] = {
    "cache-control", "content-type",  "content-length",
    "etag",          "last-modified", "expires",
};

// Stamped per response by whatever serves it, on BOTH paths, so never a
// fall-through reason.
//
// THE SEAM HANDS THIS PREDICATE FINALIZED SERVER HEADERS, NOT ORIGIN ONES --
// see the gathering loop in daemon_ipro_recorder.cc -- so this list is what
// stands between the rule and marking every response a normal server produces.
// Each member is here because the SERVING side puts it back:
//
//   date, server        the serving layer stamps both on every response it
//                       emits, this substrate's included.  `Date` is not even
//                       the origin's here: the recording seam sets it itself.
//   age                 emitted on EVERY serve from this substrate, computed
//                       from the entry rather than copied -- so an origin
//                       `Age` (any proxy- or CDN-fronted origin sends one) is
//                       not lost, it is superseded by a fresher and more
//                       accurate one.  Without this member every CDN-fronted
//                       deployment marks every response, for ever.
//   accept-ranges       classed with `Date`/`Server`/`Content-Type` by this
//                       module's own fixed-resource-header list
//                       (RewriteOptions::InitFixedResourceHeaders), i.e. as a
//                       header the server fixes for a resource response rather
//                       than one configuration may set.  It rides on
//                       essentially every static response the in-place path
//                       records, so treating it as origin content would take
//                       the substrate to zero on a default deployment.
//   connection,         hop-by-hop: never forwarded by anything, and not the
//   keep-alive,         origin's to state end to end.
//   transfer-encoding
const char* const kServeGeneratedHeaders[] = {
    "date",          "server",     "age",
    "connection",    "keep-alive", "transfer-encoding",
    "accept-ranges",
};

// The `Vary` axes a serve from this entry can honour.
//
// `accept-encoding` -- the selection is made as though the client asked for
// uncompressed bytes, so what varies on this axis is not varied over here.
//
// `accept` -- because the serve arm RECONSTRUCTS it.  An origin that
// negotiates on `Accept` is recorded with that fact on the entry
// (kPsFlagOriginVariesAccept), and the serve arm emits `Vary: Accept` back off
// it on EVERY serve of that URL.  The mechanism is worth stating exactly,
// because the obvious implementation of it is wrong: the marker is read from
// the DURABLE ORIGINAL by exact id, through the same per-request probe that
// answers rule 6, and never from the entry being served -- on an optimized
// serve that entry is a variant, and a variant's flags byte is clear by
// construction.  See ProbeOriginalFlags and the `Vary` biconditional's clause
// (ii) in daemon_serve_arm.cc, and the emission in the Apache handler.  The
// header the origin sent is therefore reproduced, which is the only question
// this predicate asks.
const char* const kHandledVaryTokens[] = {
    "accept-encoding",
    "accept",
};

template <size_t N>
bool NameIsIn(StringPiece name, const char* const (&table)[N]) {
  for (size_t i = 0; i < N; ++i) {
    if (StringCaseEqual(name, table[i])) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool OriginHeadersAreReproducible(
    const std::vector<DaemonOriginHeader>& headers) {
  // The `Vary` tokens are gathered across every `Vary` line and judged
  // together.  RFC 9110 s5.3: repeated fields are ONE list, so a per-line
  // check would pass `Vary: Accept-Encoding` and never look at the
  // `Vary: User-Agent` beside it.
  StringPieceVector vary_tokens;
  for (const DaemonOriginHeader& header : headers) {
    StringPiece name = header.name;
    TrimWhitespace(&name);
    if (StringCaseEqual(name, "vary")) {
      SplitStringPieceToVector(header.value, ",", &vary_tokens,
                               true /* omit_empty_strings */);
      continue;
    }
    if (NameIsIn(name, kReproducibleHeaders) ||
        NameIsIn(name, kServeGeneratedHeaders)) {
      continue;
    }
    // Anything not positively recognised, including a near neighbour of a
    // member: fail closed.
    return false;
  }
  for (StringPiece token : vary_tokens) {
    TrimWhitespace(&token);
    // `*` is refused by the same branch as an unhandled axis, and does not
    // need its own: it is not in the handled list and never will be.
    if (!NameIsIn(token, kHandledVaryTokens)) {
      return false;
    }
  }
  return true;
}

bool IsRewrittenUrlShape(StringPiece url) {
  return url.find(kRewrittenUrlMarker) != StringPiece::npos;
}

bool UrlIsKeySafe(StringPiece url) {
  if (url.empty() || url[0] != '/') {
    // Must be the path form: the canonical form is taken over the path, and
    // an absolute URL would compose a key with the host in it twice.
    return false;
  }
  return url.find('?') == StringPiece::npos &&
         url.find('%') == StringPiece::npos &&
         url.find('#') == StringPiece::npos;
}

DaemonRecordDecision EvaluateDaemonRecordGate(const DaemonAbi& abi,
                                              const DaemonRecordInput& input,
                                              int64 now_seconds) {
  DaemonRecordDecision decision;

  // ---- the transition guard ------------------------------------------
  // First, and before anything is derived from the response: this URL is
  // decided by its shape alone, and a gate that computed a full decision and
  // then discarded it would be one refactor away from storing first and
  // cleaning up afterwards.
  if (IsRewrittenUrlShape(input.url)) {
    decision.verdict = DaemonRecordVerdict::kRefuse;
    decision.reason = daemon_record_reason::kRewrittenUrlShape;
    return decision;
  }

  // ---- the key this write would land on -------------------------------
  if (!UrlIsKeySafe(input.url)) {
    decision.verdict = DaemonRecordVerdict::kRefuse;
    decision.reason = daemon_record_reason::kUrlNotKeySafe;
    return decision;
  }

  // ---- Vary: the PEER's predicate, not this module's ------------------
  // The two disagree, and the disagreement is not academic: this module
  // permits Vary: Cookie on validated HTML when the request carried no
  // cookie, and the peer refuses it.  The plane being written is shared, and
  // the peer's serving side reads it under the peer's key -- so an entry
  // admitted under the looser rule is one that side would hand to a client
  // its key cannot distinguish.  Strictest of both, and the peer's is the
  // strict one.
  //
  // This changes nothing about how this module treats Vary on its OWN cache;
  // that path is untouched.
  GoogleString vary_storage;
  const char* vary = CStrOrNull(input.vary, &vary_storage);
  if (abi.VaryUncacheable(vary) != 0) {
    decision.verdict = DaemonRecordVerdict::kRefuse;
    decision.reason = daemon_record_reason::kVaryNotStorable;
    return decision;
  }

  // The OTHER half of the same store-side verdict, asked of the same peer over
  // the same value: does the ORIGIN negotiate on `Accept` itself?
  //
  // Asked HERE rather than beside the write parameters below, because it is
  // the second answer to the question this block is about and the peer
  // publishes the two as halves of one classification.  Asking them apart
  // would be the first step towards asking them of different values.
  //
  // Refused responses never reach this line, which is the ordering the peer's
  // own pairing rule guarantees (a response it refuses can never vary on
  // Accept), so nothing here has to re-derive that.
  const bool origin_varies_accept = abi.VaryVariesAccept(vary) != 0;

  // ---- an encoded body is not the original ---------------------------
  // What arrives here after an upstream has encoded it is the encoded form,
  // and storing that as "the bytes the origin sent" would be false in the one
  // field the whole class is about.  Refused rather than decoded: decoding it
  // correctly is a body of work, and doing it badly is worse than not
  // recording.
  if (!input.content_encoding.empty() &&
      !StringCaseEqual(input.content_encoding, "identity")) {
    decision.verdict = DaemonRecordVerdict::kRefuse;
    decision.reason = daemon_record_reason::kContentEncoded;
    return decision;
  }

  // ---- the origin's Cache-Control state ------------------------------
  // Parsed by the peer, one line at a time, in order: several lines are one
  // list, and the peer's parser accumulates flags while a later lifetime
  // replaces an earlier one.  Doing this arithmetic here instead would be a
  // second implementation of the same RFC, disagreeing in the cases nobody
  // tests.
  PsCacheControl cc;
  memset(&cc, 0, sizeof(cc));
  cc.struct_size = sizeof(cc);
  for (const GoogleString& line : input.cache_control_lines) {
    // The return value is deliberately not consulted. The peer reports only
    // one failure here -- a null argument or an unset struct size, both of
    // which are impossible three lines up -- and it NEVER reports "this line
    // did not parse", because an unrecognised directive is a normal thing for
    // an origin to send and is meant to be ignored. So there is nothing this
    // could branch on that is not already a defect in this function, and
    // treating a line as fatal would refuse to record responses over
    // directives the peer was right to skip.
    (void)abi.ParseCacheControl(line.c_str(), &cc);
  }

  if ((cc.cc_flags & kPsCcOriginNoStore) != 0) {
    decision.verdict = DaemonRecordVerdict::kRefuse;
    decision.reason = daemon_record_reason::kOriginNoStore;
    return decision;
  }

  // ---- the content cap -----------------------------------------------
  // Applied here only when the response declared its length, so that a write
  // certain to be refused is not opened at all -- the peer's own cap is what
  // actually enforces it, including for a response whose length is unknown
  // until it ends.
  if (input.content_length > kDaemonOriginalContentCapBytes) {
    decision.verdict = DaemonRecordVerdict::kRefuse;
    decision.reason = daemon_record_reason::kOverContentCap;
    return decision;
  }

  GoogleString ct_storage;
  decision.content_type =
      abi.ClassifyContentType(CStrOrNull(input.content_type, &ct_storage));

  // ---- the write parameters ------------------------------------------
  abi.WriteParamsInit(&decision.params);
  // NOT this module's choice.  The durable-originals class has exactly one
  // id, which is what lets "the original" mean one thing, and the peer
  // refuses any other value through this entry point.  Stated explicitly
  // rather than left at the initializer's zero so that the one class this
  // module writes is visible at the point it is decided.
  decision.params.alternate_id = kPsSentinelOriginal;
  decision.params.full_mask = kPsSentinelOriginal;
  decision.params.content_type = decision.content_type;
  decision.params.content_length =
      input.content_length > 0 ? static_cast<uint64_t>(input.content_length)
                               : 0;
  // EXACTLY ONE FLAG BIT IS THIS MODULE'S TO SET, and the two cases below are
  // opposite in kind rather than two entries on a list.
  //
  // The worker-processed flag is the OPTIMIZER'S and stays clear: a front end
  // that set it would make its own recording indistinguishable from an
  // optimized variant.
  //
  // kPsFlagOriginVariesAccept is the module's, and setting it is an obligation
  // the peer documents on this write.  It records something only the writer
  // can still see: on a later hit the origin's `Vary` is gone, so if this bit
  // is not on the entry the fact is unrecoverable, and the optimizer will
  // derive a variant family from whichever representation one arbitrary
  // client's `Accept` elicited -- wrong bytes, no error, nothing downstream
  // able to notice.  The answer is the PEER'S, over the joined `Vary` this
  // gate already classified; deciding it here would be a second implementation
  // of a rule the peer owns both halves of.
  decision.params.flags = static_cast<uint8_t>(
      origin_varies_accept ? kPsFlagOriginVariesAccept : 0);
  // THE SECOND BIT, and it is a separate statement rather than a second term
  // in the one above because the two are separate obligations with separate
  // owners: the Vary-Accept answer is the PEER'S, over the joined `Vary`, and
  // this one is THIS MODULE'S, over the whole response, against a set derived
  // from what this module's own serve arm can put back.  Folding them together
  // would put a peer answer and a module answer in one expression and invite
  // the next reader to source them from the same place.
  //
  // NOT A VERDICT: a marked response is stored and notified exactly like any
  // other.  What the optimizer may do with a URL is not this module's to
  // decide from one front end's inability to serve the result -- the peer's
  // own front stage keeps a header sidecar and can serve it perfectly.  What
  // changes is what THIS module does at serve time, and that is the serve
  // arm's read of this bit.
  if (!OriginHeadersAreReproducible(input.response_headers)) {
    decision.params.flags |= kPsFlagOriginHeadersNotReproducible;
  }
  decision.params.origin_max_age = cc.max_age;
  decision.params.origin_s_maxage = cc.s_maxage;
  decision.params.origin_cc_flags = cc.cc_flags;
  decision.params.origin_last_modified = SecondsFromMs(input.last_modified_ms);

  // AGE IS ADJUSTED BY THE WRITER, and this is the whole reason the peer
  // exports the arithmetic.  A response that reached us through an upstream
  // cache carrying `Age: N` was already N seconds old on arrival; stamping
  // the local clock would silently grant it N extra seconds of freshness.
  // Behind a CDN that is systematic and invisible -- the entry just serves
  // stale bytes as fresh for the whole of Age.
  decision.params.cache_inserted_at = abi.AgeAdjustedInsertTime(
      ClampToUint32(now_seconds), ClampToUint32(input.age_seconds));

  // The two POINTER fields -- the origin's Content-Type and its ETag, both
  // carried verbatim because they belong to the origin's plane and are never
  // re-minted -- are deliberately left null here and filled in by the writer.
  // A decision is a value that outlives this frame; a pointer into this
  // frame's storage inside it would be a dangling read at the write.

  // ---- the notify mask -----------------------------------------------
  // Derived from THIS request's headers.  Not a fixed canonical mask: the
  // optimizer produces what the requesting client can use, which is what the
  // shipped front stage does and what a per-request negotiated surface
  // requires.
  GoogleString accept, user_agent, save_data, accept_encoding;
  decision.notify_mask =
      abi.Classify(CStrOrNull(input.accept, &accept),
                   CStrOrNull(input.user_agent, &user_agent),
                   CStrOrNull(input.save_data, &save_data),
                   CStrOrNull(input.accept_encoding, &accept_encoding));

  // ---- store-but-never-notify ----------------------------------------
  // no-transform is a statement about what may be done to the bytes, not
  // about whether they may be kept.  Keeping them is useful and permitted;
  // asking for them to be transformed is exactly what was forbidden.
  if ((cc.cc_flags & kPsCcOriginNoTransform) != 0) {
    decision.verdict = DaemonRecordVerdict::kStoreOnly;
    decision.reason = daemon_record_reason::kOriginNoTransform;
    return decision;
  }

  // A request whose configuration could not be stated as a value has no name
  // to file the work under.  The peer would process the notification under
  // the DEFAULT context, which is a different configuration's namespace --
  // the one outcome the context exists to prevent.  So: keep the original,
  // serve this response through, and ask for nothing.  That costs cache
  // warmth for one request; the alternative costs correctness for every
  // request that shares the URL.
  //
  // Both halves or neither, checked here rather than trusted: the peer
  // refuses a half-supplied pair, and a pair that arrived half-formed would
  // otherwise turn a missing context into a failed notification.
  if (input.option_context.empty() != input.option_signature.empty() ||
      input.option_context.empty()) {
    decision.verdict = DaemonRecordVerdict::kStoreOnly;
    decision.reason = daemon_record_reason::kNoOptionContext;
    return decision;
  }

  decision.verdict = DaemonRecordVerdict::kStoreAndNotify;
  decision.reason = daemon_record_reason::kEligible;
  return decision;
}

DaemonRecordWriter::DaemonRecordWriter(const DaemonAbi* abi, void* cache)
    : abi_(abi), cache_(cache) {}

DaemonRecordWriter::~DaemonRecordWriter() { Abandon(); }

int DaemonRecordWriter::Begin(const DaemonRecordInput& input,
                              const DaemonRecordDecision& decision) {
  if (decision.verdict == DaemonRecordVerdict::kRefuse || cache_ == nullptr) {
    return kPsErrInvalidArg;
  }
  GoogleString url, hostname, scheme, origin_ct, origin_etag;
  input.url.CopyToString(&url);
  input.hostname.CopyToString(&hostname);
  input.scheme.CopyToString(&scheme);

  // The parameters are re-pointed at storage this frame owns.  The gate's
  // copy points into ITS caller's input, which is a different object with a
  // different lifetime, and the peer reads these pointers during the call.
  PsWriteParams params = decision.params;
  params.origin_ct = CStrOrNull(input.content_type, &origin_ct);
  params.origin_etag = CStrOrNull(input.etag, &origin_etag);

  last_error_ = abi_->CacheWriteOriginal(cache_, url.c_str(), hostname.c_str(),
                                         scheme.c_str(), &params, &handle_);
  if (last_error_ != kPsOk) {
    // The peer does not hand back a handle it did not open; make sure of it
    // rather than inherit the assumption.
    handle_ = nullptr;
  }
  return last_error_;
}

int DaemonRecordWriter::Append(const void* data, size_t length) {
  if (handle_ == nullptr) {
    return last_error_ == kPsOk ? kPsErrInvalidArg : last_error_;
  }
  if (last_error_ != kPsOk) {
    return last_error_;
  }
  last_error_ = abi_->WriteData(handle_, data, length);
  return last_error_;
}

int DaemonRecordWriter::Commit() {
  if (handle_ == nullptr) {
    return last_error_ == kPsOk ? kPsErrInvalidArg : last_error_;
  }
  void* handle = handle_;
  // Cleared BEFORE the call: the peer releases the handle on every path out
  // of the commit, including the failing ones, so a member still pointing at
  // it is a use-after-free waiting for the destructor.
  handle_ = nullptr;
  const int status = abi_->WriteClose(handle);
  if (last_error_ == kPsOk) {
    last_error_ = status;
  }
  return status;
}

void DaemonRecordWriter::Abandon() {
  if (handle_ == nullptr) {
    return;
  }
  void* handle = handle_;
  handle_ = nullptr;
  abi_->WriteAbort(handle);
}

int NotifyDaemon(const DaemonAbi& abi, StringPiece socket_path,
                 const DaemonRecordInput& input,
                 const DaemonRecordDecision& decision) {
  if (decision.verdict != DaemonRecordVerdict::kStoreAndNotify) {
    return kPsErrInvalidArg;
  }
  GoogleString socket, url, hostname, scheme, context, signature;
  socket_path.CopyToString(&socket);
  input.url.CopyToString(&url);
  input.hostname.CopyToString(&hostname);
  input.scheme.CopyToString(&scheme);

  PsNotifyParams params;
  abi.NotifyParamsInit(&params);
  params.url = url.c_str();
  params.hostname = hostname.c_str();
  params.scheme = scheme.c_str();
  params.content_type = decision.content_type;
  params.mask = decision.notify_mask;
  params.agent_request = 0;
  params.option_context = CStrOrNull(input.option_context, &context);
  params.option_context_length = input.option_context.size();
  params.option_signature = CStrOrNull(input.option_signature, &signature);
  return abi.NotifyWorker(socket.c_str(), &params);
}

}  // namespace net_instaweb
