# Classification Library

Capability-based request classification, alternate metadata, and variant selection.

## Key Files
- `capability_mask.h` -- `CapabilityMask`: 32-bit bitmask encoding format, viewport, density, save-data, encoding
- `alternate_id.h` -- `AlternateId`: low 8 bits of capability mask, identifies cache alternates. Also the **sentinel registry** (`kSentinelRegistry`): every sentinel id, live or reserved, with its payload shape. New classes are registered here, and anything added from format v8 onward carries a version (its own payload version byte, or an `AlternateMetadata` prefix).
  **What the registry is today, stated precisely because this file is durable memory:** it is the single source of sentinel NAMING and RESERVATION, and the admission gate on the embedder write surface (`ps_cache_write_sentinel` refuses a *reserved* id). No read or notify path **decides** anything from it. The notify-side drop lives in `src/worker/worker.cc` and is **viewport-bits-shaped**, not registry-shaped: it rejects every mask with viewport bits = 3 apart from three whole-mask specials, so it treats a registered id exactly like an unregistered one, and that is intended to stay true — what a notification may trigger is a fixed set of specials, not "whatever this build recognises". That path *does* call `SentinelName` for its **log line**, so a namespace-skewed peer (`unknown_sentinel`) reads differently from a peer misusing a class this build knows; naming is not gating. Wiring the registry into that path as a *decision* is the work of the lanes that consume these classes — do not read "registered" as "the runtime knows about it".
- `alternate_metadata.h` -- `AlternateMetadata`: per-variant metadata. **Single source of truth for the wire format** — the exact byte layout, current version, and field semantics live in the header comment + `kCurrentVersion` constant. Do not transcribe the layout elsewhere; cite this file.
- `option_context.h` -- the **per-request options context**: the resolved
  configuration a front-end module hands us as a VALUE, plus its signature.
  Three things about it are load-bearing and easy to get wrong later, and the
  second is the one that has already been got wrong once.

  **The payload is opaque here and stays that way.** This engine validates that
  it is self-consistent, bounded and in a format version it knows, and reads
  nothing else out of it, because interpreting an option on this side would be
  re-implementing option resolution across a socket.

  **What the signature does TODAY is drift detection, not separation.** It is
  re-derived from the payload and compared byte for byte on every notification
  that carries one, and a mismatch is a counted refusal — that is the live
  behaviour. It does NOT select a cache context at this build:
  `Worker::AcceptOptionContext` accepts a valid non-default context and the
  work is processed and stored under the DEFAULT context. That is correct
  rather than a shortcut, because no value a module resolves per request
  reaches a rewriter — every input that shapes optimized output comes from the
  resource, from the request, or from the daemon's own `WorkerConfig` — so two
  contexts cannot produce two different artifacts and one stored artifact is
  right for both. Read `option_context.h`'s header comment before assuming
  otherwise; do not re-teach "keyed separation" as current behaviour.

  **The context-keyed machinery is retained but INERT.**
  `PageSpeedCache::ComposeKeyPreNormalized`'s 4-arg overload and
  `test/lib/cache/option_context_key_test.cc` exist and are green; nothing in
  the engine calls the overload. It is kept because the decision it records —
  **keyed, not filtered** (separation in the CACHE KEY, never in
  `PageSpeedSelector`) — is arithmetic rather than taste and is expensive to
  get wrong the day some output does come to depend on a resolved
  configuration.
- `pagespeed_selector.h` -- `PageSpeedSelector`: scores alternates by mask similarity for best-match selection
- `content_type.h` -- `ContentType` enum and MIME string mapping
- `hostname.h` -- hostname normalization for cache key composition
- `dimension_iterator.h` -- iterates over all capability mask dimensions for proactive variant generation

## Architecture

Request flow: HTTP headers → `CapabilityMask::FromRequest()` → `AlternateId` →
`PageSpeedSelector::select()` → best alternate or MISS.

`AlternateMetadata` serializes to a compact, version-tagged wire format. Fields:
version + full 32-bit mask + content type + flags + origin headers + SSIMULACRA2
score + content class + ETag + Last-Modified + origin content length + v7
content-binding hashes (origin HTML hash / render source hash) + v8
per-URL epoch and raw origin Cache-Control string. v9 has the v8 layout.
The format is **append-only**: each version's layout is a strict byte prefix of
the next. Older versions are accepted on read with missing fields defaulted; a
version ABOVE `kCurrentVersion` is refused outright, which the read path turns
into a MISS. Every HTML entry below v9, and Markdown versions of pages the
worker wrote below v9, are refused the same way (`IsRetired`), so they are
rebuilt. The current version (`kCurrentVersion`), exact byte offsets, and field
semantics are defined in `alternate_metadata.h` — read it before touching the format.

The header also carries the **prefix budget**: the worst-case serialized size,
derived member by member, and the `max_metadata_size` default that is bound to it
(`kRecommendedMaxMetadataSize`). A write over that ceiling is refused whole, so
adding a field without re-deriving the budget shows up in the field as entries
that are silently never stored. The derivation is pinned by `static_assert`.

The cross-version corpus in `test/lib/classify/testdata/` is real output from the
serializers that shipped at v3-v7, not transcribed layout — regenerate it from the
commits it names rather than by hand.

`PageSpeedSelector::select()` scores by: viewport (+2000), format (+1000), Original
fallback (+100), encoding (+60 exact / +5 identity), density (+40), save-data (+20; a
stored Save-Data variant for a client without Save-Data takes -980, floored at 1).
The viewport (size class) outranks format, so a client is served a variant at its
own size before one in its negotiated format at another size.
SVG gets universal +1200 bonus (resolution-independent).

## Testing
```bash
bazel test //test/lib/classify/...
```

## Gotchas
- `CapabilityMask()` default = Desktop/Identity = **0x08**, not 0x00.
- `CapabilityMask::Decode(0)` = Mobile/Identity = **0x00**.
- Mismatched non-identity encoding is a hard disqualification (score 0) in selector.
- SVG format bits (11) were formerly kJxl -- never set from Accept headers.
