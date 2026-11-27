// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — the per-request option context.
//
// WHAT IT IS.  A module in front of this engine resolves its own configuration
// per request — globals, per-host, per-directory, per-request overrides, all of
// it — and the result is a set of values.  An option context is that RESULT,
// rendered as bytes, plus a signature over those bytes.
//
// What matters here is the half this side does NOT do.  The engine does not
// merge configuration, does not inherit anything, and does not read the payload
// as policy.  It receives a value and a name for that value.  The payload is
// opaque to everything in this file: the only thing validated is that it is
// self-consistent, bounded, and in a format version this build knows.  A
// consumer that wanted to interpret an option would be re-implementing
// resolution on the wrong side of the socket, which is the failure mode this
// shape exists to make structurally impossible.
//
// WHAT THE SIGNATURE IS FOR, AT THIS BUILD.  Two things, and only the first of
// them is live:
//
//   1. DRIFT DETECTION, on every notification that carries a context.  The
//      signature is re-derived from the payload and compared byte for byte, so
//      two implementations of this format that disagree by one byte find out on
//      the first message.  A mismatch is a refusal (see OptionContextStatus).
//      This is what the worker acts on today.
//
//   2. CACHE-CONTEXT SEPARATION, described below.  The machinery exists and is
//      exercised by tests; nothing in the engine calls it yet.
//
// The reason (2) is not live is not that it is unfinished.  It is that on the
// current surface there is nothing to separate: no value a module resolves per
// request reaches a rewriter.  The capability mask and the content type are
// read off the request and the response; the agent-request bit is the
// request's own declared intent gated by an operator flag the engine itself
// publishes into shared config, not something the sender resolves; and every
// rewriter parameter comes from the engine's own daemon-side configuration,
// which no notification can touch.  So two requests
// that resolved to two different option contexts cannot produce two different
// optimized artifacts, and one stored artifact is correct for both.  The worker
// therefore ACCEPTS a valid non-default context and processes the work under
// the DEFAULT context (Worker::AcceptOptionContext); keying by a signature that
// separates nothing would only split one warm cache into several cold ones.
//
// Everything below describes (2): the shape separation takes the day some
// output does come to depend on a resolved configuration.  It is kept, and kept
// tested, because the choice it records — key, do not filter — is the expensive
// one to get wrong, and the arithmetic behind it does not change with time.
//
// WHAT THE SIGNATURE IS FOR, WHEN IT SEPARATES.  It separates cache contexts,
// EXACTLY.  Two requests that resolved to different options must not share
// optimized output, and the comparison that decides it is byte equality on the
// signature — never a nearest fit, never a bucketing of "close enough"
// contexts.
//
// WHERE THE SEPARATION HAPPENS, and why it is not per entry.  It is in the
// CACHE KEY (see PageSpeedCache::ComposeKeyPreNormalized's four-argument
// overload, which is that machinery and is currently called only by its
// tests).  The alternative —
// recording a signature on each stored alternate and filtering at selection
// time — is not available, and the reason is arithmetic rather than taste:
//
//   * A stored alternate is addressed by (cache key, one byte).  All eight
//     bits of that byte are already spent on the capability mask, so there is
//     no room in the address for a second dimension.
//   * The selector only ever sees that byte.  It deliberately does not read
//     per-alternate headers (lib/classify/pagespeed_selector.cc), so a stored
//     signature would not be visible where the decision is made.
//   * A key holds at most 64 alternates (cyclone kMaxAlternatesPerKey), and
//     the published budget already accounts 44 of them for a busy image URL.
//     A per-alternate context dimension multiplies that count by the number of
//     live contexts; two contexts on one image URL would exceed the ceiling
//     outright and the whole key would start refusing writes.
//
// Keying instead of filtering costs nothing in chain depth — each context gets
// its own key and each key keeps its own budget — and it makes exact match
// structural rather than a rule someone has to keep enforcing.
//
// THE DEFAULT CONTEXT IS THE OLD KEY, BYTE FOR BYTE.  A peer that sends no
// option context, or one whose context is the empty context, gets exactly the
// key this engine has always used.  So nothing already stored moves, no cache
// goes cold on upgrade, and a deployment that never sends a context is
// unaffected in every observable way.

#ifndef PAGESPEED_LIB_CLASSIFY_OPTION_CONTEXT_H_
#define PAGESPEED_LIB_CLASSIFY_OPTION_CONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

// The token that opens every canonical payload.  A payload that does not begin
// with it is from a format this build does not read.
inline constexpr std::string_view kOptionContextFormatVersion = "psoc1";

// Upper bound on a payload.
//
// MIRRORED, not chosen here: the producing side derives this number from the
// largest payload its option table can express and pins it with a test.  The
// two constants must agree, and the shared golden-vector file is what a
// reviewer checks them against.  If they ever disagree, the failure is a
// rejected notification and an unoptimized resource — never a truncated
// payload, because nothing anywhere truncates one.
inline constexpr size_t kMaxOptionContextBytes = 16384;

// A rendered signature: SHA-256 as lowercase hex.
inline constexpr size_t kOptionContextSignatureChars = 64;

// The signature of the empty context — SHA-256 of the format version token
// followed by one newline, which is the whole payload when nothing is set.
//
// This is a constant of the FORMAT and is shared with the producing side; it
// is the first entry of the shared golden-vector file precisely so that the
// two copies can be checked against one document.  If the two sides ever
// disagreed about this value, every default-context request would look
// non-default to one of them -- so every one of them would be counted as a
// non-default acceptance, and the day context keying goes live they would all
// key into a second namespace.  That is why a test pins it against a locally
// computed hash rather than trusting the literal.
inline constexpr std::string_view kDefaultOptionContextSignature =
    "c0908a1c05a3d6b96d4ea614f60993a83b1eabc4cf7dccfd2899610749554f01";

// The verdict on a received option context.
//
// kOk and kEmpty are usable; the four below them are refusals.  Every refusal
// means the same thing operationally — the notification carrying it is dropped
// and nothing is optimized for it — and they are distinguished because they
// have different fixes.  A size refusal is a configuration that outgrew the
// bound; a format refusal is a peer from another release; a mismatch is two
// implementations of one contract that have drifted, which is the one that
// needs a human.
enum class OptionContextStatus : uint8_t {
  kOk = 0,
  kEmpty = 1,               // No context supplied; the default context applies.
  kTooLarge = 2,            // Payload exceeded kMaxOptionContextBytes.
  kUnknownFormat = 3,       // Payload did not open with a known version token.
  kMalformedSignature = 4,  // Signature was not 64 lowercase hex characters.
  kSignatureMismatch = 5,   // Signature did not match the payload it came with.
};

// Lowercase-hex SHA-256 over `payload`.  Exactly kOptionContextSignatureChars
// characters.
std::string OptionContextSignature(std::string_view payload);

// True when `signature` names the empty context — including when it is empty,
// which is how a peer says "I did not supply one".
bool IsDefaultOptionContext(std::string_view signature);

// Checks that a received (payload, signature) pair is usable.
//
// It re-derives the signature from the payload rather than trusting the one on
// the wire.  That is not defence against a hostile peer — the socket is
// already trusted — it is defence against the two implementations of this
// format drifting apart in a way that would otherwise be silent.  A drift shows
// up here as a counted refusal on the very first message instead of as two
// deployments quietly keying the same configuration two different ways.
//
// An empty payload with an empty signature is kEmpty, not an error: it is how
// every peer that predates this field, and every request that simply has no
// override to declare, presents itself.
OptionContextStatus ValidateOptionContext(std::string_view payload,
                                          std::string_view signature);

// A human-readable name for a status, for logs and counters.
std::string_view OptionContextStatusName(OptionContextStatus status);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_OPTION_CONTEXT_H_
