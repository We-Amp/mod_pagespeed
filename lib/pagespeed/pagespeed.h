// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_H_
#define PAGESPEED_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- export / attribute macros ---------- */

#if defined(_WIN32) && defined(PS_BUILDING_SHARED)
#define PS_EXPORT __declspec(dllexport)
#elif defined(_WIN32)
#define PS_EXPORT __declspec(dllimport)
#else
#define PS_EXPORT
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PS_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define PS_NODISCARD _Check_return_
#else
#define PS_NODISCARD
#endif

/* ---------- version ---------- */

/* Adding entry points is a MINOR bump: every function that existed at 1.0 is
 * still present with the same signature, so a consumer built against 1.0 keeps
 * linking. 1.1 adds ps_serve_stats_record_serve_class and
 * ps_serve_stats_snapshot. 1.2 adds the origin-state write fields and their
 * read accessors, the freshness / Cache-Control / Vary-storability entry
 * points, and ps_write_params_init_sized. 1.3 adds ps_cache_write_original.
 * 1.4 adds ps_cache_write_headers_sidecar and ps_headers_sidecar_classify.
 * 1.5 adds the per-request options context: ps_notify_worker_ex with its
 * ps_notify_params_t, and ps_option_context_signature. 1.6 adds
 * ps_read_shared_config_volume_size and ps_cache_config_init_sized. 1.7 adds
 * ps_vary_varies_accept and publishes PS_FLAG_ORIGIN_VARIES_ACCEPT. 1.8
 * publishes PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE. 1.9 adds
 * ps_html_config_init_sized and ps_critical_css_config_init_sized. 1.10
 * adds ps_read_shared_config_generation. 1.11 adds
 * ps_serve_stats_record_hit_host. 1.12 adds the HTML cacheability report:
 * ps_html_store_classify, ps_html_decision_format and
 * ps_html_store_reason_name; the split of the serve saving by encoding:
 * ps_read_encoded_origin_length and ps_serve_stats_record_hit_split; and
 * site identity: PS_SITE_ID_CHARS, the PS_NOTIFY_FLAG_* bits, the `site`
 * and `notify_flags` fields of ps_notify_params_t, and
 * ps_cache_read_best_site, ps_cache_read_alternate_site and
 * ps_cache_write_original_site; and the `scheme` field of ps_html_config_t.
 * 1.13 adds the site-scoped HTML store surface: the `origin_html_hash` field
 * of ps_write_params_t, ps_cache_write_headers_sidecar_site with its
 * ps_sidecar_write_params_t, ps_headers_sidecar_read and the
 * ps_header_block_* accessors, ps_cache_remove_site,
 * ps_cache_write_site_purge_epoch and ps_cache_read_site_purge_epoch, and
 * publishes PS_SENTINEL_MARKDOWN_WITHHELD, PS_FLAG_HEADERS_CLASSIFIED and
 * PS_FLAG_HEADERS_SIDECAR_REQUIRED.
 *
 * A published constant is ABI surface too (see ABI.md), so a minor that adds
 * only one still moves the number: it is what a consumer compiles against to
 * know the constant is there.
 *
 * The rules this triple obeys, and the CI gate that enforces them, are
 * documented in lib/pagespeed/ABI.md. In short: removing or renaming an
 * exported symbol, or moving a field within a published struct, is a MAJOR
 * bump; adding a symbol, or appending a field to a struct that carries
 * struct_size, is a MINOR bump. lib/pagespeed/abi/abi-golden.json is the
 * checked-in record of the current surface and is what the gate compares
 * against. */
#define PS_API_VERSION_MAJOR 1
#define PS_API_VERSION_MINOR 13
#define PS_API_VERSION_PATCH 0

PS_EXPORT int ps_version_major(void);
PS_EXPORT int ps_version_minor(void);
PS_EXPORT int ps_version_patch(void);
PS_EXPORT const char* ps_git_commit(void);
PS_EXPORT const char* ps_product_version(void);

/* Async-CSS loader: the same-origin path at which the CSP-safe loader script
 * must be served, and the loader JS itself. Single source of truth shared by
 * the nginx module and the in-process .NET middleware so both front-ends serve
 * byte-identical content. Both return null-terminated UTF-8 strings. */
PS_EXPORT const char* ps_async_css_loader_path(void);
PS_EXPORT const char* ps_async_css_loader_js(void);

/* ---------- error codes ---------- */

typedef enum {
  PS_OK = 0,
  PS_ERR_NOT_FOUND = 1,
  PS_ERR_IO = 2,
  PS_ERR_CORRUPTED = 3,
  PS_ERR_NO_SPACE = 4,
  PS_ERR_INVALID_ARG = 5,
  PS_ERR_BUSY = 6,
  PS_ERR_CLOSED = 7,
  PS_ERR_TOO_MANY_ALTERNATES = 8,
  PS_ERR_EXISTS = 9,
  PS_ERR_NOT_OWNED = 10,
  PS_ERR_VERSION_MISMATCH = 11,
  PS_ERR_INTERNAL = 99,
  /* Published C API identifier: the double underscore is regrettable,
     but renaming it now would be a breaking API change.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_ERR__FORCE_INT = 0x7FFFFFFF
} ps_error_t;

PS_EXPORT const char* ps_error_name(ps_error_t err);
PS_EXPORT const char* ps_strerror(ps_error_t err);
PS_EXPORT const char* ps_last_error_message(void);

/* ---------- content types ---------- */

typedef enum {
  PS_CONTENT_HTML = 0,
  PS_CONTENT_CSS = 1,
  PS_CONTENT_JS = 2,
  PS_CONTENT_IMAGE = 3,
  PS_CONTENT_OTHER = 4,
  /* Published C API identifier: the double underscore is regrettable,
     but renaming it now would be a breaking API change.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_CONTENT__FORCE_INT = 0x7FFFFFFF
} ps_content_type_t;

PS_EXPORT ps_content_type_t
ps_classify_content_type(const char* content_type_header);

PS_EXPORT const char* ps_content_type_mime(ps_content_type_t type);

/* ---------- capability mask ---------- */

PS_EXPORT uint32_t ps_classify(const char* accept, const char* user_agent,
                               const char* save_data,
                               const char* accept_encoding);

/* 1 if the Accept header ranks text/markdown at least as high as text/html
 * (the agent-optimize negotiation signal), else 0.  text/markdown must be
 * named explicitly with a q above 0 and at least the q text/html resolves to;
 * a wildcard never selects it, equal q selects Markdown, and a malformed
 * header selects HTML.
 * Pass several Accept lines joined with ", ".  The capability mask has no free
 * bit, so this is standalone. */
PS_EXPORT int ps_wants_agent_markdown(const char* accept);

PS_EXPORT uint32_t ps_mask_set_viewport_from_width(uint32_t mask,
                                                   uint16_t width_px);

/* ---------- alternate scoring ---------- */

PS_EXPORT int ps_score_alternate(uint32_t client_mask, uint32_t stored_mask);

/* ---------- hostname normalization ---------- */

PS_EXPORT int ps_normalize_hostname(const char* hostname, char* out_buf,
                                    size_t buf_size);

/* ---------- sentinel IDs ---------- */

/* The durable ORIGINAL of a resource: the bytes the origin sent, before any
 * optimization, kept so the optimized variants can be rebuilt without
 * re-fetching. Listed here because reading it by id is part of the API —
 * ps_cache_read_alternate with this id returns it — but NOT writable through
 * ps_cache_write_sentinel, which refuses it. Use ps_cache_write_original: the
 * class carries entry metadata and a content cap that a raw sentinel write
 * would skip. */
#define PS_SENTINEL_ORIGINAL 0x0C

#define PS_SENTINEL_EARLY_HINTS 0x1C
#define PS_SENTINEL_WARMUP 0x2C
#define PS_SENTINEL_CONTENT_HASH 0x3C
#define PS_SENTINEL_SUBRESOURCE 0x4C
#define PS_SENTINEL_BROWSER_PROFILE 0x5C

/* The response-header sidecar: ONE entry per URL holding the curated,
 * verbatim block of request-independent response headers an optimized entry
 * has to reproduce. Listed here because reading it by id is part of the API,
 * but NOT writable through ps_cache_write_sentinel — use
 * ps_cache_write_headers_sidecar, which is where the admission gate lives.
 * The stored blob begins with a payload format version byte; a reader that
 * does not know the version must treat the entry as absent rather than guess
 * at the bytes behind it. */
#define PS_SENTINEL_HEADERS_SIDECAR 0x6C

/* The withheld marker: the record that the Markdown copy of a page was
 * permanently refused for one stored original, so a serve path answers
 * "withheld" and stops asking for it. It is never a Markdown entry and never
 * readable as one: the id is sentinel-shaped, so ps_cache_read_best (any mask,
 * any agent flag) never returns it, and only a reader that asks for this id
 * gets it. Listed so a serve path can read it by id, with
 * ps_cache_read_alternate_site(..., PS_SENTINEL_MARKDOWN_WITHHELD, &r); NOT
 * writable through ps_cache_write_sentinel. A durable original written for
 * the key (ps_cache_write_original, ps_cache_write_original_site) removes the
 * key's marker in the same call. Available from PS_API 1.13. */
#define PS_SENTINEL_MARKDOWN_WITHHELD 0xCC

/* ---------- flags ---------- */

/* Stored-variant flag bits (ps_write_params_t::flags, ps_read_flags). */
#define PS_FLAG_NEEDS_REVALIDATION 0x01
/* Set on variants the optimizer produced, clear on originals a front end
 * stored itself. Half of the serve-stats gate; see ps_read_is_worker_processed
 * for the accessor that reads it back. */
#define PS_FLAG_WORKER_PROCESSED 0x02
/* The origin negotiates on `Accept` ITSELF: it sent `Vary: Accept`, meaning IT
 * picks the representation from the request. A stored entry carrying this bit
 * is never handed to the optimizer — deriving a variant family from it would
 * transcode whichever representation the FIRST requester's `Accept` happened
 * to elicit and serve it to everyone else.
 *
 * YOU MUST SET IT when your store-side `Vary` classification says the origin
 * varies on `Accept`; ps_vary_varies_accept answers exactly that question.
 * The optimizer cannot re-derive it later, because on a cache hit the origin's
 * own headers are gone — the bit on the entry IS the record. Leaving it clear
 * on a response that carried `Vary: Accept` is what produces the wrong-bytes
 * outcome above, and nothing downstream can detect the omission.
 *
 * Available from PS_API 1.7. */
#define PS_FLAG_ORIGIN_VARIES_ACCEPT 0x04

/* The origin response carried headers this entry cannot reproduce. Set it at
 * record time, when the response's complete header set is still in hand and
 * ps_headers_sidecar_classify can be asked: a PS_SIDECAR_FALL_THROUGH or
 * PS_SIDECAR_NEVER_OPTIMIZED verdict means at least one header falls outside
 * the set a later serve can reproduce, and that is exactly what this bit
 * records. (Those two verdicts, not "anything that is not SWAP_ELIGIBLE" —
 * the call also returns -1 for a malformed argument list, which is a bug in
 * the call and not a statement about the response.) On a hit the origin's
 * headers are gone, so nothing downstream can re-derive it.
 *
 * WHERE THE BIT LIVES, which decides how you read it back. It is on the ONE
 * entry the writer stamped, and nowhere else:
 *
 *   - Optimizer-produced variants do NOT carry it. Every optimizer write path
 *     composes the flags byte of what it stores, and none of them forwards
 *     this bit — so every derived variant reads back with this bit clear,
 *     whatever the entry it was optimized from carried.
 *   - A durable original (ps_cache_write_original) is never returned by
 *     ps_cache_read_best — that class is excluded from selection by design.
 *     So a serve path that wants the marker must ask for the entry by id:
 *     ps_cache_read_alternate(..., PS_SENTINEL_ORIGINAL, &r) then
 *     ps_read_flags(r). Reading it off a best-fit result instead gets you an
 *     unmarked variant and a false all-clear.
 *   - If you stamped it on an ordinary entry through ps_cache_write_begin, it
 *     lives at that alternate id — including the default-mask id the
 *     optimizer's own output uses. Such a marker is visible on the hits before
 *     optimization lands and gone afterwards, because the optimizer's write
 *     replaces the entry with one whose flags it composed itself.
 *
 * ADVISORY, and it gates nothing in the library: admission stores a marked
 * response exactly as an unmarked one, scoring never reads the flags byte, and
 * the optimizer neither refuses nor prefers a marked entry. What a reader may
 * conclude is only that answering from that entry would drop headers the
 * origin sent. A serve path that cannot reproduce them serves its plain path
 * instead, which turns a silent fidelity gap into a visible fall-through.
 * Leaving the bit clear costs nothing but that visibility — unlike
 * PS_FLAG_ORIGIN_VARIES_ACCEPT, omitting it cannot produce wrong bytes.
 *
 * One effect is observable wherever a marked entry is actually served: the
 * flags byte feeds the weak ETag of a hit, so a marked entry does not validate
 * against an otherwise identical unmarked one. That is deliberate — the two
 * are served differently, so they are not the same representation.
 *
 * Available from PS_API 1.8. */
#define PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE 0x08

/* The writer ran the response through the headers-sidecar gate when it
 * recorded the entry. Together with PS_FLAG_HEADERS_SIDECAR_REQUIRED it puts
 * an entry in one of three states: neither bit (recorded without the gate;
 * serve as before), CLASSIFIED alone (headers checked, nothing to carry), and
 * CLASSIFIED plus SIDECAR_REQUIRED (a non-empty sidecar was stored for the
 * key; a serve path must read and replay it, and must treat a sidecar it
 * cannot read as a miss).
 *
 * STAMPED BY THE WRITER at record time. The optimizer copies both bits into
 * every variant it derives from the entry, except the Markdown copy. Both
 * bits feed the weak ETag of a hit, like every flag bit.
 *
 * The optimizer never sets either bit on an HTML durable original: it copies
 * a front end's own recording into PS_SENTINEL_ORIGINAL only for images. So a
 * PS_SENTINEL_ORIGINAL of HTML content type carrying
 * PS_FLAG_HEADERS_CLASSIFIED was written by a front end's own HTML store.
 * SIDECAR_REQUIRED is meaningful only together with CLASSIFIED.
 *
 * Available from PS_API 1.13 (the bits themselves are older; 1.13 publishes
 * them). */
#define PS_FLAG_HEADERS_CLASSIFIED 0x10
#define PS_FLAG_HEADERS_SIDECAR_REQUIRED 0x20

/* ---------- origin Cache-Control flags ---------- */

/* Bitfield of the origin's Cache-Control directives, as stored with an entry
 * and as consumed by ps_evaluate_freshness / ps_build_cache_control. Produce
 * it with ps_parse_cache_control rather than by hand: the two directives that
 * take an optional argument need their form recorded, and getting that wrong
 * is the difference between "this field is private" and "this response is
 * private".
 *
 * PS_CC_ORIGIN_HEADER_PRESENT answers "did the origin send Cache-Control at
 * all", which is what the content-type freshness defaults turn on — it is not
 * a directive.
 *
 * The *_BARE / *_QUALIFIED pairs distinguish `private` from
 * `private="Set-Cookie"` (RFC 9111 §5.2.2.7) and `no-cache` from
 * `no-cache="Set-Cookie"` (§5.2.2.4). Decision rule for a consumer that acts
 * destructively on the directive: BARE set means blanket (act); QUALIFIED set
 * with BARE clear means qualified only (do not act); neither set means the
 * flags predate both bits, so treat as blanket and fail safe. */
#define PS_CC_ORIGIN_NO_CACHE 0x0001u
#define PS_CC_ORIGIN_MUST_REVALIDATE 0x0002u
#define PS_CC_ORIGIN_NO_STORE 0x0004u
#define PS_CC_ORIGIN_PRIVATE 0x0008u
#define PS_CC_ORIGIN_PUBLIC 0x0010u
#define PS_CC_ORIGIN_IMMUTABLE 0x0020u
#define PS_CC_ORIGIN_S_MAXAGE_PRESENT 0x0040u
#define PS_CC_ORIGIN_PROXY_REVALIDATE 0x0080u
#define PS_CC_ORIGIN_NO_TRANSFORM 0x0100u
#define PS_CC_ORIGIN_HEADER_PRESENT 0x0200u
#define PS_CC_ORIGIN_PRIVATE_QUALIFIED 0x0400u
#define PS_CC_ORIGIN_PRIVATE_BARE 0x0800u
#define PS_CC_ORIGIN_NO_CACHE_QUALIFIED 0x1000u
#define PS_CC_ORIGIN_NO_CACHE_BARE 0x2000u
/* More than one s-maxage directive was seen (the value kept is the last).
 * Such a response is not treated as explicitly shareable. */
#define PS_CC_ORIGIN_S_MAXAGE_REPEATED 0x4000u

/* ---------- cache ---------- */

typedef struct ps_cache_s ps_cache_t;

typedef struct {
  size_t struct_size;
  const char* volume_path;
  uint64_t volume_size;
  int enable_checksum;
  size_t ram_cache_size;
  size_t max_metadata_size;
} ps_cache_config_t;

/* Fills *config with defaults.
 *
 * Legacy initializer — writes the PS_API 1.8 prefix (48 bytes) of defaults.
 * NOT SIZE-AWARE: it writes sizeof(ps_cache_config_t) AS THIS LIBRARY DEFINES
 * IT, and every field that library knows about, before the caller can inspect
 * anything.  A caller compiled against an older, SHORTER ps_cache_config_t
 * therefore has its buffer overrun by a newer library — the struct_size field
 * this writes reports the damage, it does not prevent it.  Callers that may
 * meet a newer library must use ps_cache_config_init_sized, or pass a buffer
 * large enough to absorb growth. */
PS_EXPORT void ps_cache_config_init(ps_cache_config_t* config);

/* Fills *config with defaults, writing at most `size` bytes.
 *
 * This is the size-aware entry point ps_cache_config_init is not, and it is
 * what a caller whose ps_cache_config_t may be older/shorter than the
 * library's must call: pass sizeof(ps_cache_config_t) as YOUR build sees it
 * and no byte beyond it is touched.  Fields the caller's struct is too short
 * to hold are simply not set; fields it has that this library does not know
 * about are zeroed.
 *
 * A `size` below sizeof(size_t) is refused outright and the struct is left
 * untouched: below the width of struct_size there is nowhere to record the
 * size, and stamping it would be the overrun this exists to prevent — the same
 * rule ps_write_params_init_sized and ps_notify_params_init_sized follow.
 *
 * SCOPE: this bounds what the INITIALIZER writes, nothing more.
 * ps_cache_open does not consult struct_size and reads every field this
 * library defines, so a caller compiled against a shorter struct must still
 * hand ps_cache_open a buffer large enough for THIS library's layout (an
 * over-allocated buffer is the portable spelling).
 *
 * Available from PS_API 1.6. */
PS_EXPORT void ps_cache_config_init_sized(ps_cache_config_t* config,
                                          size_t size);

PS_EXPORT PS_NODISCARD ps_error_t ps_cache_open(const ps_cache_config_t* config,
                                                ps_cache_t** out_cache);

PS_EXPORT void ps_cache_close(ps_cache_t* cache);

/* ---------- cache reads ---------- */

typedef struct ps_read_result_s ps_read_result_t;

PS_EXPORT PS_NODISCARD ps_error_t
ps_cache_read_best(ps_cache_t* cache, const char* url, const char* hostname,
                   const char* scheme, uint32_t mask, ps_read_result_t** out);

/* ---------- site identity (PS_API 1.12) ---------- */

/* Number of characters in a site id, not counting a terminating NUL. A site
 * id is the SHA-256 of a front end's server-scope cache fragment, written as
 * this many lowercase hex characters ([0-9a-f]). It names one server block or
 * virtual host, and scopes every cache key and notification for it. */
#define PS_SITE_ID_CHARS 64

/* The _site entry points: the same as the function without the suffix, in
 * the keyspace of one site.
 *
 * `site` NULL or "" is exactly the function without the suffix: the same
 * keys, byte for byte. A site id (PS_SITE_ID_CHARS lowercase hex) reads or
 * writes only that site's entries, which no other site and no unsited caller
 * can see. Any other value is PS_ERR_INVALID_ARG, and nothing is read or
 * written.
 *
 * Available from PS_API 1.12. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_read_best_site(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* site, uint32_t mask,
    ps_read_result_t** out);

/* Agent-aware best read.  Routes through the SINGLE audited
 * agent serve gate (the operator's agent_optimize flag + the content-hash
 * binding check), so a stale/unbound markdown variant is refused (returns
 * PS_ERR_NOT_FOUND) — a native/.NET caller can never serve superseded
 * markdown.  agent_entitled MUST be derived from
 * ps_read_shared_config_agent_entitled() (the worker-owned source of truth)
 * AND ps_wants_agent_markdown(), never hard-coded. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_read_best_agent(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, uint32_t mask, int agent_entitled,
    ps_read_result_t** out);

/* Read the worker-written serve-side agent_optimize toggle
 * from the shared config under cache_path.  1 = agent_optimize is on; 0 = off
 * (incl. a missing/unreadable config — the safe default); -1 = invalid
 * argument.  Since 2.1 the toggle is simply the operator's
 * --agent-optimize flag as published by the worker — there is no token
 * entitlement behind it; the function keeps its name for ABI stability.  This
 * is the native source of truth for a .NET-fronted process (no SharedConfig
 * reader otherwise). */
PS_EXPORT int ps_read_shared_config_agent_entitled(const char* cache_path);

/* The volume size, in bytes, the worker opened its cache with — read from the
 * shared config under cache_path.  Returns 0 when the size is NOT KNOWN: no
 * shared config, an unreadable one, one written by a worker predating this
 * field, or a declared schema version this build cannot read.
 *
 * 0 means "do not guess".  A caller that opens the same volume must MATCH this
 * size, because the volume's on-disk filename is derived from its geometry: a
 * caller that opens the same directory with a different size does not collide
 * with the worker, it silently creates and uses a DIFFERENT file, shares
 * nothing, and runs a permanently cold cache with no error on either side.
 * Substituting a default for a 0 return reintroduces exactly that failure, so
 * a caller that gets 0 must decline to open rather than pick a number.
 *
 * cache_path must be the SAME STRING the worker was configured with (a file
 * path — the shared-config file lives beside it, in its parent directory).
 * Passing the cache DIRECTORY resolves one level too high, finds nothing, and
 * returns 0 — which degrades safely but presents as "the worker has not
 * published a size" rather than as a caller error.
 *
 * Available from PS_API 1.6.  A consumer that must also work against an older
 * library should resolve this symbol optionally and treat its absence the same
 * as a 0 return. */
PS_EXPORT uint64_t ps_read_shared_config_volume_size(const char* cache_path);

/* The cache-directory generation (N in
 * /var/cache/pagespeed-optimizer/v<N>) the worker published in the shared
 * config under cache_path as the `cache_dir_generation` key.  Returns 0
 * when the generation is NOT KNOWN: no shared config, an unreadable one,
 * one written by a worker predating this field, or a declared schema
 * version this build cannot read.
 *
 * A caller whose own compiled-in generation differs from a non-zero return
 * here MUST treat it as a handshake failure (loud log, daemon substrate
 * down), never as a hint to go looking for another directory: two
 * generations sharing nothing is the safe-by-construction state, and the
 * loud signal is the only thing that keeps it from presenting as a site
 * that mysteriously never optimizes.
 *
 * Same cache_path contract as ps_read_shared_config_volume_size: the SAME
 * STRING the worker was configured with (a file path, not the directory).
 *
 * Available from PS_API 1.10.  A consumer that must also work against an
 * older library should resolve this symbol optionally and treat its absence
 * the same as a 0 return. */
PS_EXPORT uint32_t ps_read_shared_config_generation(const char* cache_path);

PS_EXPORT PS_NODISCARD ps_error_t ps_cache_read_alternate(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, uint8_t alternate_id, ps_read_result_t** out);

/* ps_cache_read_alternate in the keyspace of one site; see
 * ps_cache_read_best_site. Available from PS_API 1.12. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_read_alternate_site(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* site, uint8_t alternate_id,
    ps_read_result_t** out);

PS_EXPORT PS_NODISCARD ps_error_t ps_cache_read_early_hints(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, ps_read_result_t** out);

PS_EXPORT PS_NODISCARD ps_error_t
ps_read_content(const ps_read_result_t* result, const uint8_t** out_data,
                size_t* out_length);

PS_EXPORT PS_NODISCARD ps_error_t ps_read_copy(const ps_read_result_t* result,
                                               void* buf, size_t buf_size,
                                               size_t* out_copied);

PS_EXPORT uint32_t ps_read_mask(const ps_read_result_t* result);

PS_EXPORT ps_content_type_t
ps_read_content_type(const ps_read_result_t* result);

PS_EXPORT const char* ps_read_origin_content_type(
    const ps_read_result_t* result);

/* The variant's stamped origin_html_hash (32 bytes, the
 * content binding), or NULL if unbound (all-zero / pre-v7 metadata).  The
 * returned pointer is valid for the lifetime of `result`. */
PS_EXPORT const uint8_t* ps_read_origin_html_hash(
    const ps_read_result_t* result);

PS_EXPORT uint8_t ps_read_flags(const ps_read_result_t* result);

PS_EXPORT int ps_read_is_ram_hit(const ps_read_result_t* result);

PS_EXPORT uint32_t ps_read_cache_inserted_at(const ps_read_result_t* result);

/* Origin (unoptimized) content length recorded at worker write time.
 * Returns result->result.metadata.origin_content_length, 0 if result==NULL.
 * 0 also means "not available" (older metadata, or front-end-written
 * originals). Used by the serve-stats gate. */
PS_EXPORT uint32_t
ps_read_origin_content_length(const ps_read_result_t* result);

/* The size of the origin bytes encoded the way the entry is served (1.12):
 * the entry's own transfer encoding at the level the worker used, so a front
 * end can tell what the served encoding saves apart from what the rewrite
 * saves. For an identity-encoded entry it is the origin content length. On
 * success writes it to *out and returns 1. Returns 0 (and leaves *out alone)
 * when the entry does not carry it (written before this existed, or too large
 * to represent), when the origin length is unknown, or when an argument is
 * NULL. A front end passes a 0 result on as "not known" and must not use a
 * guess in its place. */
PS_EXPORT int ps_read_encoded_origin_length(const ps_read_result_t* result,
                                            uint64_t* out);

/* ---------- stored origin cache state ----------
 *
 * The origin's freshness and validator state as it was recorded when the entry
 * was stored. Together with ps_read_cache_inserted_at these are exactly the
 * inputs ps_evaluate_freshness and ps_build_cache_control take, so a front end
 * can decide reuse, revalidation and the emitted Cache-Control from a cache
 * hit alone — without keeping a second copy of the origin's headers and
 * without re-deriving RFC 9111 for itself.
 *
 * All return 0 / NULL when `result` is NULL, and 0 / NULL is also how an entry
 * stored before the field existed, or by a writer that did not set it, reads
 * back. Zero is therefore "not recorded", not "the origin said zero" — the one
 * exception being origin_max_age, where the two are only distinguishable via
 * PS_CC_ORIGIN_HEADER_PRESENT in the flags. */

/* Origin `max-age` in seconds. */
PS_EXPORT uint32_t ps_read_origin_max_age(const ps_read_result_t* result);

/* Origin `s-maxage` in seconds. Meaningful only when the flags carry
 * PS_CC_ORIGIN_S_MAXAGE_PRESENT — a shared cache prefers it over max-age,
 * a private cache must ignore it (RFC 9111 §5.2.2.9). */
PS_EXPORT uint32_t ps_read_origin_s_maxage(const ps_read_result_t* result);

/* Origin Cache-Control directives as a PS_CC_ORIGIN_* bitfield. */
PS_EXPORT uint16_t ps_read_origin_cc_flags(const ps_read_result_t* result);

/* Origin `Last-Modified` as a Unix timestamp, 0 if the origin sent none. */
PS_EXPORT uint32_t ps_read_origin_last_modified(const ps_read_result_t* result);

/* Origin `ETag`, stored verbatim — quotes and any `W/` prefix included, so it
 * can be placed in an `If-None-Match` unchanged. NULL when the origin sent
 * none. The returned pointer is valid for the lifetime of `result`. */
PS_EXPORT const char* ps_read_origin_etag(const ps_read_result_t* result);

/* Returns 1 if the variant was written by the worker (the kFlagWorkerProcessed
 * flag bit is set), else 0; 0 if result==NULL. Half of the serve-stats gate
 * (the other half being origin_content_length > 0). Exposed as a dedicated
 * accessor for FFI-boundary readability across the .NET P/Invoke boundary. */
PS_EXPORT int ps_read_is_worker_processed(const ps_read_result_t* result);

/* Re-stamp the read lease pinning this result's mmap borrow
 *.  A holder keeping `result` alive longer than 3T/4 of the
 * cache's read_lease_duration (default T=5s -> 3.75s) MUST call this at a
 * cadence <= 3T/4, keep the total hold under the cache's lease_wrap_ceiling
 * (default 60s), or copy the bytes via ps_read_copy() — past the ceiling the
 * writer wraps anyway and the borrow is unprotected.  Returns 1 when a lease
 * was renewed, 0 when there is no lease to renew (RAM-cache hit, leases
 * disabled, result==NULL, or invalid handle).  Inherits the existing
 * lifetime contract: free the result before closing the cache. */
PS_EXPORT int ps_read_renew_lease(ps_read_result_t* result);

PS_EXPORT void ps_read_free(ps_read_result_t* result);

/* ---------- serve stats ---------- */

/* Opaque handle to the serve-stats mmap (.pagespeed-serve-stats). */
typedef struct ps_serve_stats_s ps_serve_stats_t;

/* Open (read-write) the serve-stats mmap for a cache path. Internally resolves
 * pagespeed::ServeStatsPath(cache_path) then pagespeed::OpenServeStats(...).
 * The worker is the sole CREATOR; this only OPENs. Returns PS_OK and sets
 * *out on success; PS_ERR_NOT_FOUND if the file doesn't exist yet (worker not
 * started — caller should open lazily / retry); PS_ERR_INVALID_ARG if
 * cache_path or out is NULL. *out is set to NULL on any error. */
PS_EXPORT PS_NODISCARD ps_error_t ps_serve_stats_open(const char* cache_path,
                                                      ps_serve_stats_t** out);

/* Record ONE worker-processed serve HIT. No-op if h==NULL. The CALLER is
 * responsible for the gate (is-worker-processed AND origin_content_length>0);
 * this just does the per-type atomic increments via the shared helper.
 * content_type maps to pagespeed::ContentType; PS_CONTENT_OTHER/unknown is a
 * no-op. `mask` is the served variant's full capability mask: for kImage hits
 * whose image-format bits == kSvg the SVG-served counter is also bumped
 * (svg.served); pass 0 to skip the SVG accounting. */
PS_EXPORT void ps_serve_stats_record_hit(ps_serve_stats_t* h,
                                         ps_content_type_t content_type,
                                         uint64_t original_bytes,
                                         uint64_t optimized_bytes,
                                         uint32_t mask);

/* Record ONE worker-processed serve HIT exactly as ps_serve_stats_record_hit
 * does, and attribute it to the host the front end served the response for
 * (1.11). `host` is `host_len` bytes, not NUL-terminated. It is reduced to a
 * host name by the host-name rule the HTTP API reference states for
 * serve_savings_by_host (case, a port and one trailing dot do not matter); a
 * NULL or empty host, or one that rule refuses, attributes the serve to
 * "other" -- as ps_serve_stats_record_hit does for every serve. No-op if
 * h==NULL. */
PS_EXPORT void ps_serve_stats_record_hit_host(ps_serve_stats_t* h,
                                              ps_content_type_t content_type,
                                              uint64_t original_bytes,
                                              uint64_t optimized_bytes,
                                              uint32_t mask, const char* host,
                                              size_t host_len);

/* Record ONE worker-processed serve HIT exactly as
 * ps_serve_stats_record_hit_host does, and also account it in the split of the
 * saving into what the served encoding saves (original_bytes minus
 * encoded_origin_bytes) and what the rewrite saves (encoded_origin_bytes minus
 * optimized_bytes) (1.12). encoded_origin_bytes is the value
 * ps_read_encoded_origin_length returned; a front end that did not get one
 * calls ps_serve_stats_record_hit_host instead, and the serve then counts in
 * the totals but not in the split. No-op if h==NULL. */
PS_EXPORT void ps_serve_stats_record_hit_split(
    ps_serve_stats_t* h, ps_content_type_t content_type,
    uint64_t original_bytes, uint64_t encoded_origin_bytes,
    uint64_t optimized_bytes, uint32_t mask, const char* host, size_t host_len);

/* Serve-class outcome for ONE response the front end judged optimizable.
 * Exactly one class per response: an optimized serve, or origin bytes for one
 * of four reasons. The values are disjoint single bits on purpose — combining
 * two of them yields a value that is not a class at all, so the recorder drops
 * it rather than silently attributing the serve to a third class. */
typedef enum {
  PS_SERVE_CLASS_OPTIMIZED = 1,         /* served a worker-produced alternate */
  PS_SERVE_CLASS_ORIGINAL_COLD = 2,     /* origin bytes; no entry for the key */
  PS_SERVE_CLASS_ORIGINAL_PENDING = 4,  /* origin bytes; entry exists, no
                                        * acceptable alternate yet */
  PS_SERVE_CLASS_ORIGINAL_DECLINED = 8, /* origin bytes; a negative verdict is
                                         * recorded for the key */
  PS_SERVE_CLASS_ORIGINAL_SKEW = 16,    /* origin bytes; a version/handshake
                                         * fail-safe fired */
  /* Widens the value set to int so that an unrecognised class arriving from a
     caller stays unrecognised: a narrower enum would make such a value both
     undefined behaviour to load and liable to truncate onto a valid class,
     which is precisely the mis-attribution the disjoint bits exist to prevent.
     Published C API identifier: the double underscore is regrettable,
     but it matches ps_error_t / ps_content_type_t above.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_SERVE_CLASS__FORCE_INT = 0x7FFFFFFF
} ps_serve_class_t;

/* Flags orthogonal to the serve class, OR-ed together. Unknown bits are
 * ignored, so a newer caller may pass a flag this library does not know. */
#define PS_SERVE_FLAG_NONE 0u
/* The serve would have asked the worker to optimize but the caller's own
 * cooldown/dedup suppressed the request. Counted separately from the class:
 * a suppressed notify still has exactly one serve class. */
#define PS_SERVE_FLAG_NOTIFY_SUPPRESSED 1u

/* Record ONE classified serve. No-op if h==NULL. Increments exactly one
 * serve-class counter, plus the suppressed-notify counter when `flags` carries
 * PS_SERVE_FLAG_NOTIFY_SUPPRESSED. Call it once per response; calling it twice
 * for one response breaks the partition the counters exist to provide.
 *
 * A `cls` outside the enumerated set — including two classes combined — records
 * no serve class. The write is dropped, and counted in
 * serve_class_unrecognized_total so that the resulting gap in the partition is
 * visible rather than silent. Unknown flag bits do not cost the serve its
 * class; they are ignored and counted in serve_flags_unrecognized_total. */
PS_EXPORT void ps_serve_stats_record_serve_class(ps_serve_stats_t* h,
                                                 ps_serve_class_t cls,
                                                 uint32_t flags);

/* Snapshot of the serve-class + saturation block, by value.
 *
 * Versioned with the struct_size convention used by ps_write_params_t: set
 * struct_size to sizeof(ps_serve_stats_snapshot_t) as your build sees it, and
 * a newer library fills only the prefix your build knows about. Fields are
 * append-only and never reordered.
 *
 * Values, not a pointer into the mapping, so a caller never holds a borrow into
 * a file the worker may recreate under it. */
typedef struct {
  size_t struct_size;
  uint64_t serve_optimized_total;
  uint64_t serve_original_cold_total;
  uint64_t serve_original_pending_total;
  uint64_t serve_original_declined_total;
  uint64_t serve_original_skew_total;
  uint64_t notify_suppressed_total;
  /* Writes the recorder could not interpret. A non-zero
   * serve_class_unrecognized_total means writes were DROPPED: the five class
   * counters above no longer account for every classified response, so any
   * fraction derived from them is understated by this count — treat it as an
   * anomaly. serve_flags_unrecognized_total is the benign counterpart: those
   * writes were accepted with their class counted, and only unknown flag bits
   * were ignored, which is the intended behaviour when the writer is newer
   * than this library. */
  uint64_t serve_class_unrecognized_total;
  uint64_t serve_flags_unrecognized_total;
  /* Sum of the worker's sampled in-flight backlog, and the number of samples in
   * that sum. Mean backlog over an interval = delta(accum) / delta(count).
   * If delta(count) is 0 the mean is UNDEFINED for that interval (the worker
   * was not sampling) — do not divide, and do not read it as zero load. */
  uint64_t saturation_sample_accum;
  uint64_t saturation_sample_count;
  /* Worker pool width, for normalising the mean into a ratio. 0 means no
   * worker has stamped this file: the whole block is uninstrumented, and the
   * zeros above are absence of data, not absence of events. */
  uint32_t worker_pool_threads;
  /* Highest sampled backlog since the file was created (a max, not a last). */
  uint32_t saturation_hwm;
} ps_serve_stats_snapshot_t;

/* Read the serve-class + saturation block. Returns PS_ERR_INVALID_ARG if h or
 * out is NULL, or if out->struct_size is not set to at least the size of the
 * first field. Fills min(out->struct_size, library's own size) bytes and
 * leaves out->struct_size reporting how much was written. */
PS_EXPORT PS_NODISCARD ps_error_t ps_serve_stats_snapshot(
    const ps_serve_stats_t* h, ps_serve_stats_snapshot_t* out);

/* Unmap and free a serve-stats handle. No-op if h==NULL. */
PS_EXPORT void ps_serve_stats_close(ps_serve_stats_t* h);

/* ---------- cache writes ---------- */

typedef struct ps_write_handle_s ps_write_handle_t;

/* Parameters for one cache write.
 *
 * Versioned with the struct_size convention: `struct_size` is the size of the
 * struct AS THE CALLER'S BUILD SEES IT, and the library reads only that many
 * bytes. Fields are append-only and never reordered, so an older caller and a
 * newer library interoperate in both directions — the library sees zeros for
 * the fields the caller does not have, and ignores any it does not know.
 *
 * Everything below origin_ct was added at PS_API 1.2 and records the ORIGIN
 * STATE of the response being stored. A writer that leaves them zero gets
 * exactly the pre-1.2 behaviour. */
typedef struct {
  size_t struct_size;
  uint8_t alternate_id;
  uint64_t content_length;
  uint32_t full_mask;
  ps_content_type_t content_type;
  uint8_t flags;
  const char* origin_ct;

  /* --- appended at PS_API 1.2 --- */

  /* Origin `ETag`, stored verbatim (quotes and any `W/` prefix included), or
   * NULL. Copied during the call; the caller may free it afterwards. */
  const char* origin_etag;

  /* When this entry became fresh, as a Unix timestamp — and it is the WRITER'S
   * JOB to have adjusted it.
   *
   * The value must be the time the ORIGIN generated the response, not the time
   * this process stored it: if the response arrived through an upstream cache
   * carrying `Age: N`, it was already N seconds old on arrival, and storing
   * the local clock silently grants it N extra seconds of freshness. Behind a
   * CDN that is systematic, not occasional, and it is invisible — the entry
   * simply serves stale bytes as fresh for the whole of Age.
   *
   * Compute it as ps_age_adjusted_insert_time(time(NULL), inbound_age), which
   * applies the same clamping the engine's own front stage applies.
   *
   * 0 means "not supplied": the library stamps its own current time,
   * UNADJUSTED, which is the pre-1.2 behaviour and is correct only when the
   * response came straight from the origin. Any writer that can see an `Age`
   * header should set this field. */
  uint32_t cache_inserted_at;

  /* Origin `max-age` / `s-maxage`, in seconds. */
  uint32_t origin_max_age;
  uint32_t origin_s_maxage;

  /* Origin `Last-Modified` as a Unix timestamp, 0 if absent. */
  uint32_t origin_last_modified;

  /* Origin Cache-Control directives, a PS_CC_ORIGIN_* bitfield. Derive it with
   * ps_parse_cache_control. */
  uint16_t origin_cc_flags;

  /* Reserved; must be zero. Present so the struct's size is the same on every
   * compiler rather than whatever tail padding each one chooses. */
  uint8_t _reserved[6];

  /* --- appended at PS_API 1.13 --- */

  /* The SHA-256 (32 bytes) of the bytes being stored, stamped on the entry as
   * its content binding and read back with ps_read_origin_html_hash. NULL, or
   * 32 zero bytes, means unbound — the pre-1.13 behaviour. Copied during the
   * call. A headers sidecar written with the same hash
   * (ps_cache_write_headers_sidecar_site) is paired with this entry. */
  const uint8_t* origin_html_hash;
} ps_write_params_t;

/* Zero `size` bytes at `params` and stamp struct_size = size.
 *
 * ALWAYS pass sizeof(ps_write_params_t) as YOUR build sees it. This is the
 * initializer to use: it is the only one that stays correct when the struct
 * grows, because the size crosses the ABI boundary instead of being assumed.
 * Available from PS_API 1.2. */
PS_EXPORT void ps_write_params_init_sized(ps_write_params_t* params,
                                          size_t size);

/* Footgun-free spelling: takes the size from the caller's own type, so it
 * cannot drift from the struct actually allocated. Use this. */
#define ps_write_params_init_auto(params) \
  ps_write_params_init_sized((params), sizeof *(params))

/* Legacy initializer, kept for callers compiled before PS_API 1.2.
 *
 * It initializes ONLY the fields that existed at 1.1 and stamps struct_size to
 * match, so it can never write past a caller whose struct is the older,
 * smaller one — which is precisely why it cannot initialize the fields added
 * since.
 *
 * WHAT GOES WRONG IF YOU USE IT FROM NEW CODE, since it is quiet: the stamped
 * struct_size is the 1.1 size, so the library reads only that prefix. Setting
 * origin_etag / cache_inserted_at / origin_max_age / origin_s_maxage /
 * origin_last_modified / origin_cc_flags after calling this leaves them in the
 * struct and DROPS them at the write — no error, no truncation, an entry
 * simply stored without the origin state you thought you gave it. Use
 * ps_write_params_init_auto and the fields take effect. */
PS_EXPORT void ps_write_params_init(ps_write_params_t* params);

/* Fold an inbound `Age` header into an insertion timestamp: returns the time
 * the origin generated the response, given the current time and the `Age` the
 * response arrived with (0 when there was none).
 *
 * `age_seconds` is ignored when it is 0 or larger than `now_seconds`, the
 * second case being a clock or origin anomaly where subtracting would produce
 * a timestamp in a different epoch — failing toward the local clock there
 * keeps the entry merely over-fresh rather than nonsensical.
 *
 * Exported because every port that stores a response behind a CDN needs this
 * one line to be identical to the engine's, and a port that quietly skips it
 * produces entries that look fresh and are not. */
PS_EXPORT uint32_t ps_age_adjusted_insert_time(uint32_t now_seconds,
                                               uint32_t age_seconds);

PS_EXPORT PS_NODISCARD ps_error_t
ps_cache_write_begin(ps_cache_t* cache, const char* url, const char* hostname,
                     const char* scheme, const ps_write_params_t* params,
                     ps_write_handle_t** out);

/* Write a sentinel entry.
 *
 * `sentinel_id` must be sentinel-shaped, and must not name a claimed entry
 * class. Two kinds are refused, both returning PS_ERR_INVALID_ARG: a class
 * that is RESERVED — its format is fixed but its reader and writer do not
 * exist yet, so writing there means the class arrives to find foreign bytes
 * under its own id — and a class that has its own write entry point, whose
 * rules a raw sentinel write would skip (PS_SENTINEL_ORIGINAL: use
 * ps_cache_write_original; PS_SENTINEL_HEADERS_SIDECAR: use
 * ps_cache_write_headers_sidecar, whose rules are the admission gate itself,
 * so a raw write there stores exactly the header blocks the gate refuses).
 * Sentinel-shaped ids that name no class at all stay
 * writable: a free slot is yours to use, a claimed one is not. The ids
 * writable here are PS_SENTINEL_EARLY_HINTS, PS_SENTINEL_WARMUP,
 * PS_SENTINEL_CONTENT_HASH, PS_SENTINEL_SUBRESOURCE and
 * PS_SENTINEL_BROWSER_PROFILE.
 *
 * RE-RECORDING REPLACES: writing a sentinel whose id is already stored for
 * the URL removes the previous entry before the new one is written, so with
 * one writer per URL the chain keeps one node and a read always returns the
 * most recent entry. Concurrent writers for the same URL and id can leave
 * superseded entries behind — the removal and the write are separate
 * operations with no atom across them — bounded by the storage layer's chain
 * ceiling; the next storage-layer advance makes replacement unconditional
 * with no change on your side.
 *
 * REPLACEMENT WINDOW: this is a streaming write, so the removal happens when
 * you call this and the replacement exists only when you close the handle. An
 * abandoned write, or one the storage layer refuses at close (it can — at its
 * chain ceiling, or out of space), leaves the URL with no entry under that id
 * until the next store. That reads as a miss, never wrong bytes; if keeping a
 * stale entry matters more to you than the gap, read for one before you
 * start. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_write_sentinel(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, uint8_t sentinel_id, uint64_t content_length,
    ps_write_handle_t** out);

/* Write the durable ORIGINAL of a resource — the bytes the origin sent, before
 * any optimization — so that optimized variants can be rebuilt later without
 * going back to the origin. Available from PS_API 1.3.
 *
 * Takes the same ps_write_params_t as ps_cache_write_begin and uses the same
 * fields, with two differences:
 *
 *   - `alternate_id` is not yours to choose. Leave it 0 (or set it to
 *     PS_SENTINEL_ORIGINAL); any other value is PS_ERR_INVALID_ARG. The class
 *     has exactly one id, which is what lets "the original" mean one thing.
 *   - `full_mask`'s low byte must be 0 or PS_SENTINEL_ORIGINAL; the stored
 *     entry always describes itself as this class.
 *
 * The entry is NOT selectable: no request, of any shape, can be served it by
 * ordinary cache selection. It is read back deliberately, by id, with
 * ps_cache_read_alternate(..., PS_SENTINEL_ORIGINAL).
 *
 * HOW LONG IT LIVES is the origin's decision, not a fixed retention: supply
 * the origin's Cache-Control state and validators in the params exactly as you
 * would for any other write, and the entry is usable for as long as those say
 * the origin's answer stands. Leaving cache_inserted_at at 0 stamps the local
 * clock, as elsewhere.
 *
 * SIZE LIMIT: 16 MB. A larger response is not stored as a durable original —
 * above that size the copy costs more than the rebuild it saves. There is no
 * way to change it from this API, and no shipped configuration sets it, so
 * treat it as a fixed property of the library.
 *
 * Declaring more than the limit returns PS_ERR_NO_SPACE here; writing more
 * than you declared fails the ps_write_data call that crosses it and abandons
 * the entry, after which ps_write_close returns PS_ERR_CLOSED because there is
 * nothing left to commit. Nothing partial is ever stored, and no other part of
 * your response handling is affected. All three carry a ps_last_error_message
 * that names the cap — worth checking, because PS_ERR_NO_SPACE also means the
 * cache volume is genuinely full, which is transient and worth retrying, while
 * this is permanent for this response and is a normal outcome to log.
 *
 * RE-RECORDING REPLACES, with the same scope the header sidecar's block
 * carries and one extra consequence:
 *
 *   - A read always gets the most recent original, under any interleaving.
 *   - With ONE WRITER per URL, re-recording replaces: the previous original
 *     is removed before the new one is written.
 *   - With CONCURRENT WRITERS for the same URL, superseded originals can
 *     accumulate — the removal and the write are separate operations with no
 *     atom across them, and the depth does not come back down by itself. It
 *     is bounded (the storage layer stops accepting alternate writes for that
 *     key at its chain ceiling, which is loud rather than silent), but for
 *     THIS class each superseded node holds a whole response body, so it
 *     costs volume as well as depth. If you write originals for the same URL
 *     from several processes, serialise them. Unconditional replacement
 *     arrives with the next storage-layer advance and needs no change on your
 *     side.
 *
 * REPLACEMENT WINDOW, and it is wider here than anywhere else because this is
 * a streaming write: the previous original is removed when you call this, and
 * the new one exists only when you CLOSE the handle. For the whole duration
 * of your stream there is no stored original for that URL, and a stream you
 * abandon, one whose close the storage layer refuses (it can — at its chain
 * ceiling, or out of space), or one the size limit cuts off leaves none. A
 * reader in that
 * window gets a miss, which correctly means "no original is stored"; the cost
 * is a re-fetch, never wrong bytes. If keeping a stale original matters more
 * to you than the gap, read for one before you start writing and decide.
 *
 * `Vary: Accept` OBLIGATION — the one thing this call cannot do for you, and
 * the one whose omission is silent. If the response's `Vary` says the ORIGIN
 * negotiates on `Accept` (ask ps_vary_varies_accept), you MUST set
 * PS_FLAG_ORIGIN_VARIES_ACCEPT in params->flags. The optimizer treats a
 * durable original as an input it may derive a variant family from, and that
 * flag is the ONLY thing that stops it: on a hit the origin's `Vary` is gone,
 * so the bit on the entry is the whole record. Omit it and every later client
 * is served variants transcoded from whichever representation the FIRST
 * requester's `Accept` elicited — wrong bytes, no error, nothing downstream
 * able to notice. Responses ps_vary_uncacheable refuses must not be stored at
 * all, here or anywhere.
 *
 * HEADER-FIDELITY MARKER, the other thing only you can answer while the
 * response's headers are in hand: if its complete header set carries anything
 * outside what a later serve can reproduce — ask ps_headers_sidecar_classify,
 * where a PS_SIDECAR_FALL_THROUGH or PS_SIDECAR_NEVER_OPTIMIZED verdict means
 * it does (those two verdicts; -1 is a malformed call, not a statement about
 * the response) — set PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE in
 * params->flags. Unlike the `Vary` marker this one gates nothing: the entry
 * is stored and optimized identically either way. It exists so that a serve
 * path can fall through to its plain path instead of answering with headers
 * it knows are incomplete — which works only if the serve path reads the bit
 * where it lives: on THIS entry, which selection never returns, so it must be
 * read back by id. See "WHERE THE BIT LIVES" in the
 * PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE block above.
 *
 * CONCURRENCY CONSTRAINT — read this before building a producer on it. A
 * durable original committed by THIS call is not fenced against a concurrent
 * purge of the same URL. The engine's own purge fencing is internal to the
 * optimizer process: it stops the optimizer's tasks from writing across a
 * purge, and it cannot see your write at all. So a store that commits after a
 * purge has enumerated the URL's entries survives with its family gone — an
 * original outliving the response it belongs to. Nothing in the shipped engine
 * produces originals yet, so this cannot arise today; a production writer of
 * this class has to close it (a cross-process fence, or re-checking after the
 * write) rather than inherit an assumption that was never about it. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_write_original(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const ps_write_params_t* params,
    ps_write_handle_t** out);

/* ps_cache_write_original in the keyspace of one site; see
 * ps_cache_read_best_site. Available from PS_API 1.12. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_write_original_site(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* site, const ps_write_params_t* params,
    ps_write_handle_t** out);

/* ---------- response-header sidecar ---------- */

/* What the sidecar's admission gate decided about a response. */
typedef enum {
  /* Every header the response carries is either reproduced by the entry
     metadata, stamped fresh by the serving stack, or carried verbatim in the
     sidecar. The response may be optimized in place. */
  PS_SIDECAR_SWAP_ELIGIBLE = 0,
  /* At least one header cannot be reproduced, so the response is served
     plain — correctly, just not optimized in place. A normal outcome. */
  PS_SIDECAR_FALL_THROUGH = 1,
  /* A Content-Security-Policy carrying a nonce. Not a gap to be closed
     later: a nonce is per-response by construction, so replaying it would
     defeat the policy, and this class is never optimized. */
  PS_SIDECAR_NEVER_OPTIMIZED = 2,
  /* Pins the enum to int width, like every other published enum here. It is
     load-bearing for this one in particular: the verdict crosses the boundary
     as an `int` (the return of ps_headers_sidecar_classify and the type of
     ps_cache_write_headers_sidecar's out-param), so an implementation that
     narrowed the enum would make the two disagree about the size of the same
     value.
     Published C API identifier: the double underscore is regrettable,
     but it matches ps_error_t / ps_content_type_t above.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_SIDECAR__FORCE_INT = 0x7FFFFFFF
} ps_sidecar_verdict_t;

/* Classify a complete origin response header set — the admission gate on its
 * own, with no cache involved. Available from PS_API 1.4.
 *
 * `names[i]` / `values[i]` are the response's header lines AS RECEIVED, one
 * entry per line: repeated field names are separate entries, and joining them
 * changes the answer. Names are matched ASCII-case-insensitively; values are
 * not interpreted except where the gate says so.
 *
 * Returns a ps_sidecar_verdict_t value, or -1 when `count` is non-zero and
 * `names` or `values` is NULL, or an element is NULL. It does NOT touch the
 * last-error message: it takes no cache handle, sets no error, and reports
 * its one failure mode in the return value, so there is nothing of its own to
 * report — and clearing would discard a pending message from an earlier call
 * that the caller has not read yet. Same shape as ps_vary_uncacheable.
 *
 * The gate is FAIL-CLOSED: a field name it does not positively recognise is a
 * reason to fall through, never a header quietly dropped. Which headers the
 * sidecar carries is a closed set and is not configurable — a header whose
 * value depends on the request must never be stored, and "which ones those
 * are" is not a per-caller decision. */
PS_EXPORT int ps_headers_sidecar_classify(const char* const* names,
                                          const char* const* values,
                                          size_t count);

/* Offer a response's headers to the sidecar: ONE entry per URL holding the
 * curated, verbatim block of request-independent headers an optimized entry
 * has to reproduce. Available from PS_API 1.4.
 *
 * Pass the response's COMPLETE header set, in the form
 * ps_headers_sidecar_classify takes. You do not curate it — this call is the
 * admission gate, which is why PS_SENTINEL_HEADERS_SIDECAR is not writable
 * through ps_cache_write_sentinel.
 *
 * A REFUSAL IS NOT AN ERROR. Most responses carry something the entry cannot
 * reproduce; those are served plain. Such a call returns PS_OK with
 * `*out_stored` 0 and the verdict that explains it, and an error return means
 * the cache itself failed. `*out_stored` is also 0, with a swap-eligible
 * verdict, when the response has nothing for the sidecar to carry — every
 * header it sent is already reproduced by the entry metadata.
 *
 * `out_verdict` and `out_stored` may each be NULL.
 *
 * WHAT "ONE ENTRY PER URL" MEANS, precisely, because the obvious reading is
 * stronger than what the library can deliver today:
 *
 *   - A read ALWAYS gets the most recent block, under any interleaving. This
 *     is the property to build on.
 *   - With ONE WRITER per URL, a re-store REPLACES: the previous block is
 *     removed before the new one is written and nothing accumulates.
 *   - With CONCURRENT WRITERS for the same URL, superseded blocks CAN
 *     accumulate. The removal and the write are separate operations with no
 *     atom across them, so two calls can interleave and leave two, and the
 *     depth does not come back down by itself. It is bounded — the storage
 *     layer stops accepting alternate writes for that key at its chain
 *     ceiling, which is a loud failure rather than a silent one — but if you
 *     write this class from several processes for the same URL, serialise
 *     them. Unconditional replacement arrives with the next storage-layer
 *     advance and needs no change on your side.
 *
 * A REMOVAL WINDOW comes with it, and a serve path must inherit one rule from
 * it: between the removal and the commit a reader finds NO block, and if the
 * write then fails the previous block is gone with nothing in its place. A
 * missing block must therefore mean "this response is not optimizable" and
 * never "optimized, with no headers" — otherwise that window is exactly the
 * bug this class exists to prevent.
 *
 * Read it back by id with
 * ps_cache_read_alternate(..., PS_SENTINEL_HEADERS_SIDECAR): what you get is
 * the WHOLE payload, starting with its format version byte. That the entry
 * metadata parse does not consume any of it is guaranteed, not incidental —
 * this class's format version is constrained to a range no entry-metadata
 * version can occupy, and that constraint is part of the ABI (see ABI.md). A
 * reader that does not know the version byte must treat the entry as absent.
 *
 * CONCURRENCY CONSTRAINT, the same one ps_cache_write_original carries and
 * for the same reason: a block committed by this call is not fenced against a
 * concurrent purge of the same URL, because the engine's purge fencing is
 * internal to the optimizer process and cannot see your write.
 *
 * On PS_ERR_INVALID_ARG (a NULL cache, url, hostname, or a holed array)
 * neither out-param is written. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_write_headers_sidecar(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* const* names, const char* const* values,
    size_t count, int* out_verdict, int* out_stored);

/* ---------- site-scoped headers sidecar (PS_API 1.13) ---------- */

/* Where a sited sidecar goes, and what it is paired with.
 *
 * struct_size convention: zero the struct and set struct_size to
 * sizeof(ps_sidecar_write_params_t) as your build sees it; fields are
 * append-only. A struct_size below the 1.13 size is PS_ERR_INVALID_ARG.
 *
 * `site` is NULL or "" for the unsited keyspace, or PS_SITE_ID_CHARS lowercase
 * hex characters (see ps_cache_read_best_site). `origin_html_hash` is the
 * 32-byte stamp of the durable original this block belongs to (the
 * `origin_html_hash` that original was written with), or NULL for an unpaired
 * block. 32 zero bytes are refused: zero is the "unbound" value, and a block
 * paired with it could never be told apart from one paired with nothing. */
typedef struct {
  size_t struct_size;
  const char* url;
  const char* hostname;
  const char* scheme;
  const char* site;
  const uint8_t* origin_html_hash;
} ps_sidecar_write_params_t;

/* ps_cache_write_headers_sidecar in the keyspace of one site, optionally
 * paired with a durable original. Available from PS_API 1.13.
 *
 * Everything ps_cache_write_headers_sidecar says holds here — the gate, the
 * verdicts, a refusal being PS_OK with `*out_stored` 0, the replacement
 * window — with one addition: a PAIRED write (non-NULL `origin_html_hash`)
 * stores a block even when the response has nothing for it to carry, so that
 * "this original needs no extra headers" is a fact a reader can prove rather
 * than infer from a miss. `*out_stored` is then 1.
 *
 * ORDER, for a writer that stores both: write the sidecar FIRST and the
 * durable original second. Between the two a reader finds the new block
 * beside the old original, and pairing makes that visible
 * (PS_SIDECAR_UNPAIRED) instead of replaying the new block's headers with the
 * old bytes. A sidecar write that succeeds before a failed original write
 * stays unpaired until the next store.
 *
 * PS_ERR_INVALID_ARG for a NULL cache or params, a struct_size below the 1.13
 * size, a NULL url or hostname, a malformed site, an all-zero hash, or a holed
 * header array; neither out-param is written then. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_write_headers_sidecar_site(
    ps_cache_t* cache, const ps_sidecar_write_params_t* params,
    const char* const* names, const char* const* values, size_t count,
    int* out_verdict, int* out_stored);

/* A stored headers sidecar, parsed and owned: it holds its own copy of the
 * block, so it stays valid after the cache read that produced it, and needs
 * nothing else freed. Free it with ps_header_block_free. */
typedef struct ps_header_block_s ps_header_block_t;

/* Read the headers sidecar of a URL in one site's keyspace (`site` as for
 * ps_cache_read_best_site). Available from PS_API 1.13.
 *
 * PS_OK with `*out` set when a block this build can parse is stored, paired
 * or not. PS_ERR_NOT_FOUND when there is none — including a block in a format
 * this build does not know, which a reader must treat as absent. That case is
 * "sidecar missing"; a block that is present but belongs to a different
 * original is "sidecar unpaired", which ps_header_block_pairing answers.
 * Other errors are cache failures. `*out` is NULL on every non-PS_OK return. */
PS_EXPORT PS_NODISCARD ps_error_t ps_headers_sidecar_read(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* site, ps_header_block_t** out);

/* The number of header lines in the block; 0 for NULL. A paired block may
 * hold none. */
PS_EXPORT size_t ps_header_block_count(const ps_header_block_t* block);

/* Header line `index` (0 <= index < count), as stored: name and value
 * verbatim, each NUL-terminated (a stored value never contains NUL, CR or
 * LF), with their lengths. The pointers stay valid until the block is freed.
 * Any out-pointer may be NULL. PS_ERR_INVALID_ARG for a NULL block or an
 * index out of range; nothing is written then. */
PS_EXPORT PS_NODISCARD ps_error_t ps_header_block_get(
    const ps_header_block_t* block, size_t index, const char** out_name,
    size_t* out_name_len, const char** out_value, size_t* out_value_len);

/* The block's pairing hash (32 bytes), or NULL when the block is unpaired
 * (written without one, or by a build before 1.13) or `block` is NULL. Valid
 * until the block is freed. */
PS_EXPORT const uint8_t* ps_header_block_pairing_hash(
    const ps_header_block_t* block);

/* What ps_header_block_pairing returns. */
#define PS_SIDECAR_UNPAIRED 0
#define PS_SIDECAR_PAIRED 1

/* Whether the block belongs to the durable original whose stamp is
 * `entry_hash` (32 bytes, from ps_read_origin_html_hash on that original).
 * PS_SIDECAR_PAIRED only when the block carries a pairing hash and it equals
 * `entry_hash` byte for byte; PS_SIDECAR_UNPAIRED otherwise, including an
 * unpaired block and a NULL `entry_hash` (an unbound original), because
 * neither can prove the two belong together. -1 for a NULL block.
 *
 * Unpaired is a normal state, not a fault: every re-store passes through it
 * (new sidecar before new original), and it lasts until the original's write
 * commits. A serve path falls through on it, the same as on a missing block,
 * but counts it apart from one: a missing block where one is required is a
 * fault, an unpaired one is the expected window. */
PS_EXPORT int ps_header_block_pairing(const ps_header_block_t* block,
                                      const uint8_t* entry_hash);

/* Free a block from ps_headers_sidecar_read. No-op for NULL. */
PS_EXPORT void ps_header_block_free(ps_header_block_t* block);

/* ---------- HTML cacheability policy ---------- */

/* The `mode` ps_html_store_classify takes. Any other value is a malformed call.
 * Nothing in this API writes or serves cached HTML. */
typedef enum ps_html_cache_mode_t {
  PS_HTML_CACHE_OFF = 0,         /* refuse every response */
  PS_HTML_CACHE_REPORT_ONLY = 1, /* decide and report, never act */
  /* Pins the enum to int width, like every other published enum here.
     Published C API identifier: the double underscore is regrettable,
     but it matches ps_error_t / ps_content_type_t above.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_HTML_CACHE__FORCE_INT = 0x7FFFFFFF
} ps_html_cache_mode_t;

/* What ps_html_store_classify returns. The values 1 and 2 are reserved. */
#define PS_HTML_STORE_ACTION_REFUSE 0
#define PS_HTML_STORE_ACTION_WOULD_STORE 3
#define PS_HTML_STORE_ACTION_WOULD_STORE_NO_NOTIFY 4

/* Flag bits ps_html_store_classify writes to `*out_flags`. */
#define PS_HTML_STORE_FLAG_LIFETIME_EXPLICIT 1u
#define PS_HTML_STORE_FLAG_NEEDS_VALIDATION 2u
#define PS_HTML_STORE_FLAG_HAS_VALIDATORS 4u
#define PS_HTML_STORE_FLAG_EXPLICIT_SHARED 8u
#define PS_HTML_STORE_FLAG_SIDECAR_EVALUATED 16u
#define PS_HTML_STORE_FLAG_CC_EVALUATED 32u

/* Bits of the `request_flags` argument of ps_html_store_classify. */
#define PS_HTML_STORE_REQUEST_HAS_AUTHORIZATION 1u
#define PS_HTML_STORE_REQUEST_AUTHENTICATED_USER 2u
#define PS_HTML_STORE_REQUEST_HAS_COOKIE 4u
#define PS_HTML_STORE_REQUEST_NO_STORE 8u

/* Report whether a text/html response would be stored. Available from
 * PS_API 1.12. A pure function of its arguments: no cache handle, no I/O, no
 * clock read (the caller passes `now_seconds`, the receipt time), no global
 * state. It does NOT touch the last-error message, for the reason
 * ps_headers_sidecar_classify does not.
 *
 * `mode` is a ps_html_cache_mode_t value. `method` and `url_path` are
 * NUL-terminated. `request_flags` carries the
 * PS_HTML_STORE_REQUEST_* bits: the request carried Authorization or
 * Proxy-Authorization (1), the server authenticated the request (2), the
 * request carried a Cookie or Cookie2 header (4), the request's Cache-Control
 * carried no-store (8, RFC 9111 section 5.2.1.5). Unknown bits are ignored.
 * `names[i]` / `values[i]` are the
 * origin's response header lines AS RECEIVED, one entry per line, before any
 * header your server adds, in the form ps_headers_sidecar_classify takes (the
 * header set is run through that gate). `body_prefix` is the first
 * `body_prefix_len` bytes of the body (at most 65536 are examined; NULL with
 * length 0 means none), which is scanned for markers of a per-client page; it
 * must hold the whole body or its first 65536 bytes, whichever is less, or the
 * response is refused with the reason `incomplete` and the detail
 * `body_prefix`.
 * `max_body_bytes` is the operator's size cap, clamped to a 16 MiB ceiling.
 *
 * Returns a PS_HTML_STORE_ACTION_* value: refuse, would store, or would store
 * without notifying the optimizer (the response said no-transform). No value
 * means "store", and the caller must not write anything. A body with a
 * per-client marker is refused with the reason `personalisation_signal`, whose
 * detail names the marker. In the off mode every response is refused with the
 * reason `mode`. Returns -1 for a malformed call (a `mode` outside the
 * enumerated set, a NULL `method`, `url_path` or out-parameter other than the detail buffer;
 * NULL arrays or elements with `count` > 0; a NULL `body_prefix` with a
 * non-zero length; a NULL `detail_buf` with a non-zero capacity); no
 * out-parameter is written then.
 *
 * `*out_reason` receives the reason as an integer; ps_html_store_reason_name
 * turns it into the closed set of names (none, method, status, incomplete,
 * auth_request, cookie_request, set_cookie, private, no_store, no_lifetime,
 * vary, headers, nonce_csp, nonce_link, content_type, encoding, size, url,
 * personalisation_signal, no_transform, mode). The detail (the header name for
 * `headers`, the Vary token for `vary`, `request` for a no_store caused
 * by the request, the signal name for `personalisation_signal`) is copied into `detail_buf`, NUL-terminated and
 * truncated to `detail_capacity`; `*out_detail_len` is the untruncated length
 * (the detail is RFC 9110 token characters only, anything else having become `_`, at most 128 bytes, so a reason code built from it always fits the `reason` grammar of ps_html_decision_format). `detail_buf` may be NULL
 * with `detail_capacity` 0. `*out_lifetime_s` is the explicit freshness
 * lifetime (0 when none), `*out_flags` carries the PS_HTML_STORE_FLAG_* bits,
 * `*out_cc_flags` the parsed Cache-Control bits (PS_CC_ORIGIN_*), meaningful
 * only with PS_HTML_STORE_FLAG_CC_EVALUATED, and `*out_sidecar` the header-set
 * verdict as ps_headers_sidecar_classify reports it, meaningful only with
 * PS_HTML_STORE_FLAG_SIDECAR_EVALUATED (0 is a real verdict, so an earlier
 * refusal must not be read as one).
 *
 * The policy is strict on purpose: a response is stored only when it states
 * its own lifetime or carries a validator, never on a guessed one (two
 * max-age or two s-maxage directives conflict and count as lifetime 0); the
 * request and the response must show no sign of being personalised (a
 * request with a cookie is stored only when the response is explicitly
 * public or carries a valid s-maxage); and the only Vary
 * it admits is Accept-Encoding. Those rules are not configurable. */
PS_EXPORT int ps_html_store_classify(
    int mode, const char* method, const char* url_path, uint32_t request_flags,
    int status, const char* const* names, const char* const* values,
    size_t count, uint64_t body_bytes, int body_complete,
    const char* body_prefix, size_t body_prefix_len, uint64_t max_body_bytes,
    uint32_t now_seconds, int* out_reason, uint32_t* out_lifetime_s,
    uint32_t* out_flags, uint16_t* out_cc_flags, int* out_sidecar,
    char* detail_buf, size_t detail_capacity, size_t* out_detail_len);

/* Format one HTML cache decision as a single JSON line. Available from
 * PS_API 1.12. `keys[i]` / `values[i]` are parallel arrays of NUL-terminated
 * strings. The key set is closed (ts_ms, host, path, mode, action, reason,
 * status, size, lifetime_s, validators, cc, vary, sidecar, request,
 * personalisation, before) and has no key for a body,
 * a header value, a cookie, a client address or a user agent; the keys come out
 * in that order whatever order they are given in.
 *
 * Every value is checked against its key's grammar, which admits fixed codes
 * and counts only: ts_ms, status, size and lifetime_s take a canonical
 * decimal integer (written as a number); mode takes off or report-only; action
 * takes would_store, would_store_no_notify, skipped or refuse; reason takes a
 * name from ps_html_store_reason_name, sidecar_write_failed, or
 * `headers:<name>` /
 * `vary:<token>` with an RFC 9110 token of at most 128 bytes; validators takes
 * none or an ordered subset of etag, last_modified; request takes none or an
 * ordered subset of authorization, cookie; cc takes
 * response directive names, each at most once, with `=<integer>` only on
 * max-age, s-maxage, stale-while-revalidate and stale-if-error; vary takes a
 * comma-separated list of tokens (in cc and vary, spaces around a comma are
 * allowed, and a vary value is at most 1024 bytes); sidecar takes eligible_block,
 * eligible_empty, fall_through, fall_through:<header-name> or
 * never:nonce_csp or never:nonce_link; personalisation takes a signal name;
 * before takes a hash in decimal or lowercase hex digits; host takes hostname characters (at most 255 bytes);
 * path takes the path the cache key was built from (at most 300 bytes, never
 * `?`, `#`, `%` or `;`). A value is never empty: a fact that is absent is a key that is
 * left out.
 *
 * The line (no trailing newline) is written to `buf` NUL-terminated, and
 * `*out_len` receives its length. Returns 0 on success; -1 for a malformed
 * call (NULL arrays or elements with `count` > 0, a NULL `out_len`, a NULL
 * `buf` with a non-zero capacity); -2 when the formatter refused the fields (an
 * unknown or duplicate key, or a value outside its key's grammar, which
 * includes any control byte, an empty value, a `path` over 300 bytes or with a
 * query, an empty `host`), with `*out_len` 0; -3 when `buf` is too small,
 * with `*out_len` the length needed excluding the NUL and `buf` left holding an
 * empty string. A capacity of 0 may pass a NULL `buf` to ask only for the
 * length. Pure: it opens nothing and does not touch the last-error message. */
PS_EXPORT int ps_html_decision_format(const char* const* keys,
                                      const char* const* values, size_t count,
                                      char* buf, size_t capacity,
                                      size_t* out_len);

/* The closed-set name for a reason value ps_html_store_classify wrote, for
 * callers that log or count by name.
 * Available from PS_API 1.12. Returns a static NUL-terminated string that must
 * not be freed, or NULL for a value outside the enumerated set. */
PS_EXPORT const char* ps_html_store_reason_name(int reason);

PS_EXPORT PS_NODISCARD ps_error_t ps_write_data(ps_write_handle_t* handle,
                                                const void* data,
                                                size_t length);

PS_EXPORT PS_NODISCARD ps_error_t ps_write_close(ps_write_handle_t* handle);

PS_EXPORT void ps_write_abort(ps_write_handle_t* handle);

/* ---------- cache management ---------- */

PS_EXPORT int ps_cache_alternate_exists(ps_cache_t* cache, const char* url,
                                        const char* hostname,
                                        const char* scheme,
                                        uint8_t alternate_id);

PS_EXPORT PS_NODISCARD ps_error_t ps_cache_remove(ps_cache_t* cache,
                                                  const char* url,
                                                  const char* hostname,
                                                  const char* scheme);

/* ps_cache_remove in the keyspace of one site (`site` as for
 * ps_cache_read_best_site): the whole key — the durable original, its
 * sidecar, the withheld marker, the content-hash record, the Markdown copy
 * and every variant — in that site only. Available from PS_API 1.13.
 *
 * PS_OK when the key was removed; PS_ERR_NOT_FOUND when nothing was stored
 * under it, which a caller removing an entry may treat as success.
 *
 * PER PROCESS for the RAM tier: this process's RAM copies of the key are
 * evicted with it, but another process that already holds a copy in its own
 * RAM tier can keep answering from it. A purge must fence peer processes
 * itself (for example with the per-site purge epoch below); a removal is
 * not a cross-process invalidation.
 *
 * NOT FENCED against the optimizer: a derivation already in flight for the
 * key can still write its result after this returns. Such a copy is bound to
 * the removed original's hash, so it is not served unless an original with
 * the same bytes is stored again — and then serving it is correct. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_remove_site(ps_cache_t* cache,
                                                       const char* url,
                                                       const char* hostname,
                                                       const char* scheme,
                                                       const char* site);

/* ---------- per-site purge epoch (PS_API 1.13) ---------- */

/* The time (Unix seconds) of the last global purge a site's front end
 * observed: an entry the front end stored at or before the epoch counts as
 * purged. `site` must be PS_SITE_ID_CHARS lowercase hex; NULL and "" are
 * PS_ERR_INVALID_ARG.
 *
 * WHERE IT LIVES: one small file per site beside the cache volume,
 * `<volume_path>.purge-epoch.<site>`, where `volume_path` is the path this
 * cache was opened with. It is NOT in the volume, so the volume's own
 * reclamation cannot drop it, and a wipe of a front end's own caches leaves
 * it. ps_cache_remove and ps_cache_remove_site never touch it. It is created
 * 0660 and updated in place (never renamed or replaced), so every user of the
 * cache group can update it once it exists; the volume's directory must let
 * them create it the first time.
 *
 * WRITE IS A MAX-MERGE. Under an OS advisory lock the current value is read,
 * and only a larger value is written, as one fixed-width record at the start
 * of the file, then flushed, so racing writers — threads or processes — end
 * at the largest value. A value at or below the stored epoch is a successful
 * no-op. A value later than the current time plus 5 seconds is
 * PS_ERR_INVALID_ARG, so a clock step or a file touched with a future time
 * cannot become a permanent epoch: pass min(observed, now). PS_ERR_IO when
 * the file cannot be opened or written, or its lock is not free within about
 * 2 seconds.
 *
 * READ gives PS_OK and 0 when the site has no epoch file. ANY OTHER RESULT
 * THAN PS_OK — PS_ERR_CORRUPTED for a file that does not hold a whole record
 * (the first write was torn by a power loss, or the file is foreign),
 * PS_ERR_IO, anything —
 * means the caller must treat every entry of the site as purged; the
 * out-parameter is left untouched then and must not be used. The next write
 * replaces a corrupted record with the larger of its value and the current
 * time.
 *
 * THE EPOCH NEVER DECREASES. A stored value in the future (a power-loss tear
 * of an overwrite, which never reads below the previous value, or a write
 * made while the clock ran fast) is a valid value: it is returned as it is
 * and over-fences until the clock passes it, by design.
 *
 * The file is not tied to the volume's generation: a recreated volume holds
 * no entry from before, so an epoch that outlives the recreate can only
 * fence more than needed, never un-purge an entry. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_write_site_purge_epoch(
    ps_cache_t* cache, const char* site, uint64_t epoch_s);
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_read_site_purge_epoch(
    ps_cache_t* cache, const char* site, uint64_t* out_epoch_s);

/* ---------- cache alternate listing ---------- */

typedef struct {
  size_t struct_size;
  uint64_t content_length;
  uint64_t hit_count;
  uint8_t alternate_id;
  uint8_t _padding[7];
} ps_alternate_info_t;

PS_EXPORT PS_NODISCARD ps_error_t ps_cache_list_alternates(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, ps_alternate_info_t** out_alternates,
    size_t* out_count);

PS_EXPORT void ps_alternates_free(ps_alternate_info_t* alternates);

/* ---------- cache stats ---------- */

typedef struct {
  size_t struct_size;
  uint64_t ram_cache_hits;
  uint64_t ram_cache_misses;
  uint64_t disk_cache_hits;
  uint64_t disk_cache_misses;
  uint64_t bytes_read;
  uint64_t bytes_written;
  uint64_t evictions;
  uint64_t current_entries;
  uint64_t current_size_bytes;
  uint64_t volume_capacity_bytes;
  uint64_t ram_cache_bytes;
  uint64_t total_hits;
  uint64_t total_misses;
} ps_cache_stats_t;

/* Read the HTTP cache statistics. Returns PS_ERR_INVALID_ARG if cache or out is
 * NULL, or if out->struct_size is not set to at least the size of the first
 * field. Fills min(out->struct_size, library's own size) bytes and leaves
 * out->struct_size reporting how much was written. On rejection the struct is
 * left untouched. */
PS_EXPORT PS_NODISCARD ps_error_t ps_cache_stats(ps_cache_t* cache,
                                                 ps_cache_stats_t* out);

/* ---------- HTTP cache policy (RFC 9111) ----------
 *
 * Three pure functions, no cache handle and no I/O: given the origin state a
 * stored entry carries, may it still be served, may it be stored at all, and
 * what Cache-Control should go out with it.
 *
 * They are exported for one reason. A front end that answers these questions
 * itself ends up with a second implementation of RFC 9111 that disagrees with
 * the engine's in edge cases nobody tests — one cache deciding an entry is
 * fresh while the other decides it is stale, on the same bytes. These are the
 * engine's own answers, so there is one implementation and one dialect.
 *
 * All three are thread-safe and allocate nothing. */

/* Freshness verdict. Maps directly to front-end control flow:
 *   PS_FRESHNESS_FRESH          -> serve, with max-age = remaining_ttl
 *   PS_FRESHNESS_SERVE_NO_CACHE -> serve, but emit `Cache-Control: no-cache`
 *   PS_FRESHNESS_REVALIDATE     -> do not serve; revalidate against the origin
 *                                  (conditionally when the entry has an ETag
 *                                  or Last-Modified, otherwise a full fetch)
 *   PS_FRESHNESS_STALE_SERVE    -> RETIRED: never returned. The value is kept
 *                                  so an existing switch stays exhaustive. */
typedef enum {
  PS_FRESHNESS_FRESH = 0,
  PS_FRESHNESS_STALE_SERVE = 1,
  PS_FRESHNESS_SERVE_NO_CACHE = 2,
  PS_FRESHNESS_REVALIDATE = 3,
  /* Published C API identifier: the double underscore is regrettable,
     but it matches ps_error_t / ps_content_type_t above.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_FRESHNESS__FORCE_INT = 0x7FFFFFFF
} ps_freshness_verdict_t;

/* Is this a shared (proxy) cache or a private one?
 *
 * Zero is SHARED, deliberately. It is the engine's own default, and it is the
 * fail-safe direction: a shared cache that believes it is private will serve an
 * origin's `private` response to the next user and will ignore `s-maxage`,
 * while a private cache that believes it is shared merely revalidates more
 * often than it had to. A caller that zero-fills its input and never hears of
 * this field therefore lands on the cautious side — which is what the
 * struct_size convention requires of every field's zero.
 *
 * Any value that is not PS_CACHE_SCOPE_PRIVATE is treated as shared, so a value
 * arriving from a newer header cannot silently become "private" either. */
typedef enum {
  PS_CACHE_SCOPE_SHARED = 0,
  PS_CACHE_SCOPE_PRIVATE = 1,
  /* Published C API identifier: the double underscore is regrettable,
     but it matches ps_error_t / ps_content_type_t above.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_CACHE_SCOPE__FORCE_INT = 0x7FFFFFFF
} ps_cache_scope_t;

/* Freshness policy — the caps and the per-type defaults applied when the
 * origin sent no Cache-Control. Initialize with ps_freshness_config_init_auto
 * and override what you mean to change; passing NULL config to
 * ps_evaluate_freshness uses these same defaults. */
typedef struct {
  size_t struct_size;
  uint32_t max_age_cap;           /* default 86400 */
  uint32_t immutable_max_age_cap; /* default 604800 */
  uint32_t html_max_age;          /* default 0 */
  uint32_t css_max_age;           /* default 300, also used for JS */
  uint32_t image_max_age;         /* default 3600 */
  uint8_t _reserved[4];           /* reserved; must be zero */
} ps_freshness_config_t;

/* Zero `size` bytes at `config`, stamp struct_size = size, then fill in the
 * defaults that fit. ALWAYS pass sizeof(ps_freshness_config_t) as YOUR build
 * sees it — or use ps_freshness_config_init_auto below, which does that for
 * you.
 *
 * There is deliberately NO size-less exported form. An `..._init(&s)` that
 * clears sizeof(*s) writes the LIBRARY's idea of the size into the CALLER's
 * buffer and runs off the end of it the first time the struct grows (see
 * ABI.md). This struct is new at 1.2 and carries a reserved tail precisely
 * because it is expected to grow, so it ships with the initializer that
 * survives that rather than the one that has to be worked around later. */
PS_EXPORT void ps_freshness_config_init_sized(ps_freshness_config_t* config,
                                              size_t size);

/* Footgun-free spellings: the size comes from the caller's own type, so it
 * cannot drift from the struct the caller actually allocated. Prefer these over
 * writing sizeof by hand.
 *
 * They deliberately do NOT shadow the exported function names. A macro named
 * `ps_write_params_init` would silently change what already-written source does
 * on recompilation, and would break taking the function's address; a distinct
 * name keeps the choice visible at the call site. */
#define ps_freshness_config_init_auto(config) \
  ps_freshness_config_init_sized((config), sizeof *(config))

/* One entry's freshness question. Every field is a scalar the caller already
 * has: `now_seconds` from its clock, the rest from the read accessors above.
 *
 * PRECONDITION on cache_inserted_at: it is Age-adjusted, i.e. it is what the
 * writer stored under the contract documented on ps_write_params_t. Passing an
 * unadjusted timestamp does not fail — it silently reports stale entries as
 * fresh for as long as the response was already old on arrival. */
typedef struct {
  size_t struct_size;
  uint32_t now_seconds;
  uint32_t cache_inserted_at;
  uint32_t origin_max_age;
  uint32_t origin_s_maxage;
  ps_content_type_t content_type;
  /* Shared (proxy) cache or private. Zero = shared, which is what makes
   * `s-maxage` and `proxy-revalidate` apply. An origin-private cache — an
   * in-process middleware serving one application — sets
   * PS_CACHE_SCOPE_PRIVATE. */
  ps_cache_scope_t cache_scope;
  /* Nonzero when the CLIENT demanded revalidation (a forced reload). Yields
   * PS_FRESHNESS_REVALIDATE without marking the entry expired: the client
   * asked, which says nothing about whether the cached bytes are outdated. */
  int force_revalidate;
  uint16_t origin_cc_flags;
  uint8_t _reserved[2]; /* reserved; must be zero */
} ps_freshness_input_t;

typedef struct {
  size_t struct_size;
  ps_freshness_verdict_t verdict;
  uint32_t age_seconds;       /* how old the entry is now */
  uint32_t effective_max_age; /* lifetime actually applied, after caps */
  uint32_t remaining_ttl;     /* seconds of freshness left, 0 if stale */
  int is_stale;               /* nonzero if at or past effective_max_age */
  /* Nonzero ONLY when the entry is stale because it aged out. A forced client
   * reload and an origin `no-cache` both also yield REVALIDATE, and for both
   * this stays zero — neither means the cached bytes are outdated, and a
   * consumer that purges on staleness must key on THIS, or an ordinary browser
   * reload purges the entry set. */
  int expired_by_age;
} ps_freshness_result_t;

/* Evaluate one entry's freshness. `config` may be NULL for the defaults.
 * Returns PS_ERR_INVALID_ARG if `input` or `out` is NULL, or if either
 * struct_size is unset. */
PS_EXPORT PS_NODISCARD ps_error_t ps_evaluate_freshness(
    const ps_freshness_input_t* input, const ps_freshness_config_t* config,
    ps_freshness_result_t* out);

/* Cache mode: what the response may promise downstream. */
typedef enum {
  PS_CACHE_MODE_SAFE = 0,       /* adds must-revalidate, never adds public,
                                * never synthesizes stale-while-revalidate */
  PS_CACHE_MODE_AGGRESSIVE = 1, /* may add public and stale-if-error, and may
                                 * synthesize SWR when stale serving is
                                 * permitted */
  /* Published C API identifier: the double underscore is regrettable,
     but it matches ps_error_t / ps_content_type_t above.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_CACHE_MODE__FORCE_INT = 0x7FFFFFFF
} ps_cache_mode_t;

typedef struct {
  size_t struct_size;
  ps_cache_mode_t mode;
  /* Shared (proxy) cache or private. Zero = shared; see ps_cache_scope_t for
   * why that polarity and not the other. Gates `proxy-revalidate` and the
   * s-maxage split (RFC 9111 §5.2.2.9-10). */
  ps_cache_scope_t cache_scope;
  uint32_t effective_max_age; /* from ps_evaluate_freshness */
  uint32_t origin_max_age;    /* raw origin max-age, for the s-maxage split */
  ps_content_type_t content_type;
  uint16_t origin_cc_flags;
  uint8_t synthesize_swr; /* aggressive mode only */
  /* Relay the origin's `no-cache`. This is about FIDELITY, not storage
   * permission: dropping it turns a revalidate-before-every-use resource into
   * one freely reusable for the whole max-age. Off by default so existing
   * callers are byte-identical. */
  uint8_t relay_origin_no_cache;
  /* Forward the origin's STORAGE restrictions (`no-store`, bare `private`)
   * downstream instead of dropping them. Turn it on when the response is being
   * passed through but not stored — otherwise the next cache in the chain
   * stores exactly what this one just refused. */
  uint8_t forward_origin_restrictions;
  uint8_t _reserved[5]; /* reserved; must be zero */
} ps_cache_control_input_t;

/* Build a `Cache-Control` field value into `buf`.
 *
 * Writes AT MOST `capacity` bytes and does NOT NUL-terminate; `*out_len` is
 * how many were written. A directive that would not fit is skipped rather than
 * truncated, so the result is always a well-formed list — short, never
 * corrupt. 256 bytes is comfortably enough for every combination.
 *
 * `out_len` is required; `out_final_max_age` may be NULL. It reports the
 * max-age actually emitted, which is what an `Age` header must be computed
 * against and is not always input->effective_max_age (an origin that sent
 * `s-maxage` gets its own max-age relayed to private caches).
 *
 * Returns PS_ERR_INVALID_ARG if `input`, `buf` or `out_len` is NULL, if
 * `capacity` is 0, or if input->struct_size is unset. */
PS_EXPORT PS_NODISCARD ps_error_t ps_build_cache_control(
    const ps_cache_control_input_t* input, char* buf, size_t capacity,
    size_t* out_len, uint32_t* out_final_max_age);

/* Store-side `Vary` refusal predicate: 1 if a response carrying this `Vary`
 * must NOT be stored, 0 if it may be.
 *
 * A response may be stored only when its `Vary` names nothing but
 * `Accept-Encoding`: one stored response per URL serves every client, and the
 * coding is chosen per request. `*` or any other field (`Accept`,
 * `User-Agent`, `Save-Data`, ...) refuses it. Earlier versions also admitted
 * `User-Agent`, `Accept` and `Save-Data`.
 *
 * `vary` is the field VALUE, NUL-terminated, or NULL for a response with no
 * `Vary` (which is storable). A response carrying several `Vary` lines must
 * have them joined with `,` into one value before calling — they are one list
 * (RFC 9110 §5.3), and calling per line would accept a combination this
 * predicate as a whole refuses.
 *
 * Matching is ASCII case-insensitive with OWS trimmed; empty list members are
 * skipped.
 *
 * This is the REFUSAL half of one store-side verdict; ps_vary_varies_accept is
 * the other half. Both are thin wrappers over a single classification of the
 * same value, so they cannot disagree about it. */
PS_EXPORT int ps_vary_uncacheable(const char* vary);

/* Store-side `Vary: Accept` predicate: 1 if the ORIGIN negotiates on `Accept`
 * itself, 0 if it does not.
 *
 * This is the question PS_FLAG_ORIGIN_VARIES_ACCEPT records, and the reason
 * this predicate is exported: a writer has to answer it at store time, because
 * on a later cache hit the origin's `Vary` is gone and the answer is
 * unrecoverable. Classify the response's `Vary` here, and stamp the flag on
 * what you store when this returns 1.
 *
 * `vary` has exactly the shape ps_vary_uncacheable documents: the field VALUE,
 * NUL-terminated, several `Vary` lines joined with `,`, or NULL for none.
 *
 * ALWAYS 0 when ps_vary_uncacheable would return 1. The class is "store it,
 * but never optimize it", and a response that may not be stored at all has
 * nothing to mark — so a caller that refuses the response first never needs
 * this, and a caller that asks both gets a consistent pair.
 *
 * Since ps_vary_uncacheable refuses `Vary: Accept`, this returns 0 for every
 * value. It stays exported for callers built against it.
 *
 * Available from PS_API 1.7. */
PS_EXPORT int ps_vary_varies_accept(const char* vary);

/* Origin Cache-Control state, as produced by ps_parse_cache_control. */
typedef struct {
  size_t struct_size;
  uint32_t max_age;
  uint32_t s_maxage;
  uint16_t cc_flags;    /* PS_CC_ORIGIN_* bitfield */
  uint8_t _reserved[2]; /* reserved; must be zero */
} ps_cache_control_t;

/* Parse ONE `Cache-Control` response header line into `out`, ACCUMULATING.
 *
 * A response may carry several Cache-Control lines and RFC 9111 §5.2 treats
 * them as one list, so call this once per line, in order, against the same
 * `out`: flags are OR-ed and a later lifetime overwrites an earlier one,
 * exactly as one combined line would behave. Zero `out` (and set its
 * struct_size) before the first call.
 *
 * Every call sets PS_CC_ORIGIN_HEADER_PRESENT, so after the last one that bit
 * answers "did the origin send Cache-Control at all" — which is the question
 * the per-content-type freshness defaults turn on, and is not the same as
 * "max-age is zero".
 *
 * Returns PS_ERR_INVALID_ARG if `header_value` or `out` is NULL or if
 * out->struct_size is unset. */
PS_EXPORT PS_NODISCARD ps_error_t
ps_parse_cache_control(const char* header_value, ps_cache_control_t* out);

/* ---------- worker notification ---------- */

/* Send a fire-and-forget notification to the worker over the socket at
 * `socket_path`: new content has been recorded in the cache and is ready to
 * be optimized. `scheme` must be "http" or "https".
 *
 * Returns PS_ERR_INVALID_ARG if `socket_path`, url, hostname or scheme is
 * NULL, if scheme is neither "http" nor "https", if the content type is out
 * of range, or if the notification cannot be serialized; no connection is
 * attempted in any of those cases. PS_ERR_IO if the notification could not
 * be delivered. */
PS_EXPORT PS_NODISCARD ps_error_t ps_notify_worker(
    const char* socket_path, const char* url, const char* hostname,
    const char* scheme, ps_content_type_t content_type, uint32_t mask);

/* ---------- per-request options context (PS_API 1.5) ---------- */

/* Number of characters in a rendered options-context signature, not counting a
 * terminating NUL. A buffer passed to ps_option_context_signature must have
 * room for this many characters plus the NUL. */
#define PS_OPTION_CONTEXT_SIGNATURE_CHARS 64

/* Largest canonical options-context payload this library accepts.
 *
 * Part of the ABI because it is part of the contract, not an implementation
 * detail: a caller that builds a payload has to know the ceiling it is
 * building against, and the number is the same one the producing side derives
 * from its own option table. */
#define PS_MAX_OPTION_CONTEXT_BYTES 16384

/* Compute the signature of a canonical options-context payload.
 *
 * Writes PS_OPTION_CONTEXT_SIGNATURE_CHARS lowercase hex characters plus a
 * terminating NUL into `out`, so `out_size` must be at least
 * PS_OPTION_CONTEXT_SIGNATURE_CHARS + 1.
 *
 * Exported for the same reason ps_age_adjusted_insert_time is: every port that
 * declares an options context has to produce byte-identical signatures for
 * byte-identical payloads, forever. A port that reimplements this and drifts by
 * one byte has every notification it sends refused, because the receiver
 * re-derives the signature from the payload rather than trusting the one on the
 * wire. One implementation, exported, is cheaper than a second one that has to
 * be kept honest.
 *
 * Returns PS_ERR_INVALID_ARG if `payload` is NULL while `payload_length` is
 * non-zero, if `out` is NULL, if `out_size` is too small, or if
 * `payload_length` exceeds PS_MAX_OPTION_CONTEXT_BYTES. */
PS_EXPORT PS_NODISCARD ps_error_t ps_option_context_signature(
    const char* payload, size_t payload_length, char* out, size_t out_size);

/* Parameters for one worker notification.
 *
 * Versioned with the struct_size convention: `struct_size` is the size of the
 * struct AS THE CALLER'S BUILD SEES IT, fields are append-only, and zero means
 * "not supplied" for every one of them. Initialize with
 * ps_notify_params_init_auto.
 *
 * The fields up to and including `mask` are exactly ps_notify_worker's
 * arguments; that entry point remains and is unchanged. */
typedef struct {
  size_t struct_size;

  /* Required. */
  const char* url;
  const char* hostname;
  const char* scheme; /* "http" or "https" */
  ps_content_type_t content_type;
  uint32_t mask;

  /* Non-zero when the triggering request was an agent request with the
   * agent_optimize toggle on.  Zero is the pre-existing behaviour. */
  int agent_request;

  /* The canonical options context this request resolved to, and its
   * signature — or NULL and 0 for a caller that resolves no per-request
   * configuration, which is the pre-existing behaviour in every respect.
   *
   * SUPPLY BOTH OR NEITHER. A payload without its signature has no name; a
   * signature without its payload names something the receiver never saw.
   * Either half alone is PS_ERR_INVALID_ARG rather than a silently dropped
   * field, because the signature can only be checked against the payload it
   * came with, and that check is the whole point of carrying it.
   *
   * `option_signature` must be PS_OPTION_CONTEXT_SIGNATURE_CHARS lowercase hex
   * characters and must be the signature OF THIS PAYLOAD; the library checks
   * that rather than trusting it, and a mismatch is PS_ERR_INVALID_ARG. Derive
   * it with ps_option_context_signature and the check costs nothing.
   *
   * The payload is carried, not interpreted. This library does not merge,
   * inherit, or read policy out of it.
   *
   * WHAT THE RECEIVER DOES WITH IT, stated here so it is learned from the
   * header rather than from behaviour. A context that validates is ACCEPTED,
   * whatever it names, and the resulting work is processed and stored under
   * the DEFAULT context — the namespace a caller supplying no context gets.
   * Optimized output on this surface is a function of the resource, of the
   * request (capability mask and content type, plus the agent-request bit —
   * the request's own declared intent, gated by an operator flag the optimizer
   * itself publishes), and of the optimizer's own configuration, which a
   * notification cannot reach; no value a caller resolves per request reaches
   * a rewriter. So two requests that
   * resolved to two different contexts cannot produce two different optimized
   * artifacts, and one stored artifact is correct for both.
   *
   * The signature is nonetheless validated BYTE-EXACTLY on arrival, and a
   * payload that is oversized, in an unknown format, or accompanied by a
   * malformed or non-matching signature is REFUSED — the notification is
   * dropped before anything is read or written. That is drift detection: two
   * implementations of this format that disagree by one byte find out on the
   * first notification. KEEP SUPPLYING THE CONTEXT YOU RESOLVE. Supplying it
   * is correct today, it is what the drift check runs on, and it is what a
   * later release needs in order to separate output by configuration should
   * any output ever come to depend on one. */
  const char* option_context;
  size_t option_context_length;
  const char* option_signature;

  /* PS_API 1.12. The site the notification belongs to: NULL or "" for a
   * caller with no site (its work stays in the unsited keyspace), or
   * PS_SITE_ID_CHARS lowercase hex. Any other value is PS_ERR_INVALID_ARG. */
  const char* site;

  /* PS_API 1.12. PS_NOTIFY_FLAG_* bits; zero is unvouched and not from the
   * module. Any other bit is reserved and is PS_ERR_INVALID_ARG.
   *
   * A caller whose struct_size predates these two fields reads as site NULL
   * and notify_flags 0. */
  uint32_t notify_flags;
} ps_notify_params_t;

/* The sender checked `hostname` against the names the site serves. Work that
 * fetches from that host (page rendering for agents, /llms.txt) runs only for
 * a vouched notification. */
#define PS_NOTIFY_FLAG_HOST_VOUCHED 0x1u

/* The sender is the mod_pagespeed in-place arm. Such a notification must
 * carry a site, and must not carry PS_CONTENT_HTML, an agent request, the
 * llms.txt mask or PS_NOTIFY_FLAG_HOST_VOUCHED; each is PS_ERR_INVALID_ARG. */
#define PS_NOTIFY_FLAG_FROM_MODULE 0x2u

/* Zero `size` bytes at `params` and stamp struct_size = size. Always pass
 * sizeof(ps_notify_params_t) as YOUR build sees it — use the _auto spelling
 * below, which takes it from the caller's own type. */
PS_EXPORT void ps_notify_params_init_sized(ps_notify_params_t* params,
                                           size_t size);

#define ps_notify_params_init_auto(params) \
  ps_notify_params_init_sized((params), sizeof *(params))

/* Send a notification carrying everything ps_notify_worker sends plus the
 * agent-request bit, the per-request options context, and (1.12) the site
 * and flags.
 *
 * Returns PS_ERR_INVALID_ARG if `socket_path` or `params` is NULL, if
 * `params->struct_size` is unset or smaller than its first field, if url,
 * hostname or scheme is NULL, if scheme is neither "http" nor "https", if the
 * content type is out of range, if the options context violates any of the
 * rules documented on the struct, if the site or flags violate the rules
 * documented on the struct and on PS_NOTIFY_FLAG_FROM_MODULE, or if the
 * notification cannot be serialized; no connection is attempted in any of
 * those cases. A caller whose struct_size predates the site fields is still
 * served, unsited and unvouched; the library logs that once per process. PS_ERR_IO if
 * the notification could not be delivered. */
PS_EXPORT PS_NODISCARD ps_error_t
ps_notify_worker_ex(const char* socket_path, const ps_notify_params_t* params);

/* ---------- viewport class ---------- */

typedef enum {
  PS_VIEWPORT_MOBILE = 0,
  PS_VIEWPORT_TABLET = 1,
  PS_VIEWPORT_DESKTOP = 2,
  /* Published C API identifier: the double underscore is regrettable,
     but renaming it now would be a breaking API change.
     NOLINTNEXTLINE(bugprone-reserved-identifier) */
  PS_VIEWPORT__FORCE_INT = 0x7FFFFFFF
} ps_viewport_t;

/* ---------- HTML processing configuration ---------- */

typedef struct {
  size_t struct_size;

  int enable_critical_css;
  int enable_lazy_load;
  int enable_image_dimensions;
  int enable_lcp_preload;
  int enable_preconnect;
  int enable_speculation_rules;

  int critical_css_max_elements;
  int critical_css_max_depth;

  int css_import_max_depth;

  ps_viewport_t viewport;

  size_t max_html_size;
  size_t max_css_size;

  const char** always_include_selectors;
  const char** include_tag_patterns;
  const char** include_class_patterns;
  const char** include_id_patterns;
  const char** exclude_class_patterns;
  const char** exclude_id_patterns;
  const char** exclude_tag_patterns;

  // Opt-in: defer render-blocking stylesheets to non-blocking loading via the
  // CSP-safe external loader (requires critical CSS). APPENDED at the end of
  // the struct to preserve the ABI offsets of all fields above (the in-process
  // .NET NativeHtmlConfig mirrors this exact layout).
  //
  // Honoured by ps_html_process, which scans and gathers the page's
  // stylesheets and can therefore refuse to defer a sheet it could not
  // measure. IGNORED by ps_html_transform_create, which has neither and so
  // cannot make that judgement at all; that path always keeps stylesheets
  // render-blocking, and still inlines the critical CSS it is given.
  int enable_async_css;

  /* PS_API 1.12. The scheme of the page being processed, "http" or "https":
   * the scheme its stylesheets were written to the cache under, which
   * ps_html_process reads them back with. NULL or "" means "https". Any other
   * value makes ps_html_process fail with PS_ERR_INVALID_ARG.
   *
   * Read only when struct_size covers it. A caller whose struct_size predates
   * this field (ps_html_config_init stamps 128) reads as "https", which is
   * what every caller got before the field existed. ps_html_transform_create
   * ignores it: that entry point reads nothing from the cache by scheme. */
  const char* scheme;
} ps_html_config_t;

/* Legacy initializer: writes the defaults of exactly the PS_API 1.8 prefix
 * (128 bytes) and stamps struct_size = 128, however large this library's
 * ps_html_config_t has grown since.  It never touches `scheme` (PS_API 1.12)
 * or any later field, so ps_html_process does not read them from a config it
 * initialized.  To have `scheme` read, initialize with ps_html_config_init_auto
 * or ps_html_config_init_sized, which stamp the full size, then set it. */
PS_EXPORT void ps_html_config_init(ps_html_config_t* config);

#define ps_html_config_init_auto(config) \
  ps_html_config_init_sized((config), sizeof *(config))

/* Fills *config with defaults, writing at most `size` bytes.
 *
 * This is the size-aware entry point ps_html_config_init is not, and it is
 * what a caller whose ps_html_config_t may be older/shorter than the
 * library's must call: pass sizeof(ps_html_config_t) as YOUR build sees it
 * and no byte beyond it is touched.  Fields the caller's struct is too short
 * to hold are simply not set; fields it has that this library does not know
 * about are zeroed.
 *
 * A `size` below sizeof(size_t) is refused outright and the struct is left
 * untouched: below the width of struct_size there is nowhere to record the
 * size, and stamping it would be the overrun this exists to prevent — the same
 * rule ps_write_params_init_sized and ps_notify_params_init_sized follow.
 *
 * SCOPE: this bounds what the INITIALIZER writes, nothing more.
 * ps_html_process consults struct_size only for `scheme` (PS_API 1.12) and
 * reads every earlier field this library defines, so a caller compiled against
 * a shorter struct must still hand ps_html_process a buffer large enough for
 * the 128-byte layout (an over-allocated buffer is the portable spelling).
 *
 * Available from PS_API 1.9. */
PS_EXPORT void ps_html_config_init_sized(ps_html_config_t* config, size_t size);

/* ---------- HTML processing result ---------- */

typedef struct ps_html_result ps_html_result_t;

/* Optimizes one HTML document.  The input is read, and the output written,
 * as text/html; XHTML and other XML documents are not supported.  The page's
 * external stylesheets are read from `cache` under `hostname` and the config's
 * `scheme` ("https" when the config predates that field). */
PS_EXPORT PS_NODISCARD ps_error_t ps_html_process(
    const char* html, size_t html_len, const char* url, const char* hostname,
    const ps_html_config_t* config, ps_cache_t* cache, ps_html_result_t** out);

PS_EXPORT const char* ps_html_result_output(const ps_html_result_t* result,
                                            size_t* out_len);

PS_EXPORT int ps_html_result_modified(const ps_html_result_t* result);

PS_EXPORT int ps_html_result_has_critical_css(const ps_html_result_t* result);

PS_EXPORT const char* ps_html_result_early_hints(const ps_html_result_t* result,
                                                 size_t* out_len);

PS_EXPORT int ps_html_result_needs_revalidation(const ps_html_result_t* result);

PS_EXPORT void ps_html_result_free(ps_html_result_t* result);

/* ---------- HTML scanner ---------- */

typedef struct ps_scan_result ps_scan_result_t;

PS_EXPORT PS_NODISCARD ps_error_t ps_html_scan(const char* html,
                                               size_t html_len, const char* url,
                                               ps_scan_result_t** out);

PS_EXPORT size_t ps_scan_element_count(const ps_scan_result_t* result);

PS_EXPORT ps_error_t ps_scan_element(const ps_scan_result_t* result,
                                     size_t index, const char** out_tag,
                                     const char** out_id, int* out_depth,
                                     int* out_element_index);

PS_EXPORT size_t ps_scan_element_classes(const ps_scan_result_t* result,
                                         size_t index,
                                         const char*** out_classes);

PS_EXPORT size_t ps_scan_stylesheet_count(const ps_scan_result_t* result);

PS_EXPORT ps_error_t ps_scan_stylesheet(const ps_scan_result_t* result,
                                        size_t index, const char** out_href,
                                        const char** out_media);

PS_EXPORT const char* ps_scan_inline_css(const ps_scan_result_t* result,
                                         size_t* out_len);

PS_EXPORT const char* ps_scan_lcp_candidate(const ps_scan_result_t* result,
                                            const char** out_srcset,
                                            const char** out_sizes,
                                            int* out_element_index);

PS_EXPORT size_t ps_scan_origin_count(const ps_scan_result_t* result);

/* The scan C API intentionally exposes a subset of the origin entry: the
 * per-origin CORS bit and in-picture flag are internal to the transform and
 * not surfaced here. */
PS_EXPORT const char* ps_scan_origin(const ps_scan_result_t* result,
                                     size_t index);

PS_EXPORT void ps_scan_result_free(ps_scan_result_t* result);

/* ---------- critical CSS ---------- */

typedef struct {
  size_t struct_size;
  int max_elements;
  int max_depth;
  ps_viewport_t viewport;
  size_t max_css_size;

  const char** always_include_selectors;
  const char** include_tag_patterns;
  const char** include_class_patterns;
  const char** include_id_patterns;
  const char** exclude_class_patterns;
  const char** exclude_id_patterns;
  const char** exclude_tag_patterns;
} ps_critical_css_config_t;

/* Legacy initializer — writes the PS_API 1.8 prefix (88 bytes) of defaults.
 * NOT SIZE-AWARE: it writes sizeof(ps_critical_css_config_t) AS THIS LIBRARY DEFINES
 * IT, and every field that library knows about, before the caller can inspect
 * anything.  A caller compiled against an older, SHORTER ps_critical_css_config_t
 * therefore has its buffer overrun by a newer library — the struct_size field
 * this writes reports the damage, it does not prevent it.  Callers that may
 * meet a newer library must use ps_critical_css_config_init_sized, or pass a buffer
 * large enough to absorb growth. */
PS_EXPORT void ps_critical_css_config_init(ps_critical_css_config_t* config);

#define ps_critical_css_config_init_auto(config) \
  ps_critical_css_config_init_sized((config), sizeof *(config))

/* Fills *config with defaults, writing at most `size` bytes.
 *
 * This is the size-aware entry point ps_critical_css_config_init is not, and it
 * is what a caller whose ps_critical_css_config_t may be older/shorter than the
 * library's must call: pass sizeof(ps_critical_css_config_t) as YOUR build sees
 * it and no byte beyond it is touched.  Fields the caller's struct is too short
 * to hold are simply not set; fields it has that this library does not know
 * about are zeroed.
 *
 * A `size` below sizeof(size_t) is refused outright and the struct is left
 * untouched: below the width of struct_size there is nowhere to record the
 * size, and stamping it would be the overrun this exists to prevent — the same
 * rule ps_write_params_init_sized and ps_notify_params_init_sized follow.
 *
 * SCOPE: this bounds what the INITIALIZER writes, nothing more.
 * ps_css_extract_critical does not consult struct_size and reads every field
 * this library defines, so a caller compiled against a shorter struct must still
 * hand ps_css_extract_critical a buffer large enough for THIS library's layout
 * (an over-allocated buffer is the portable spelling).
 *
 * Available from PS_API 1.9. */
PS_EXPORT void ps_critical_css_config_init_sized(
    ps_critical_css_config_t* config, size_t size);

typedef struct ps_critical_css_result ps_critical_css_result_t;

PS_EXPORT PS_NODISCARD ps_error_t ps_css_extract_critical(
    const ps_scan_result_t* scan_result, const char* css, size_t css_len,
    const ps_critical_css_config_t* config, ps_critical_css_result_t** out);

PS_EXPORT const char* ps_critical_css_output(
    const ps_critical_css_result_t* result, size_t* out_len);

PS_EXPORT void ps_critical_css_stats(const ps_critical_css_result_t* result,
                                     int* out_total_rules,
                                     int* out_critical_rules);

PS_EXPORT void ps_critical_css_result_free(ps_critical_css_result_t* result);

/* ---------- CSS processing ---------- */

/* Returns PS_ERR_INVALID_ARG when `css` holds a NUL byte, or a `<` followed
 * by an ASCII letter, `/`, `!` or `?` anywhere, inside CSS strings and
 * comments too (so `</style` in any letter case). A bare `<`, as in a range
 * media query, is accepted. The check is strict because a caller may write
 * the CSS into a page as it is: a sheet such as Tailwind CSS v4's, whose
 * `@property` rules carry `syntax: "<color>"`, is refused here. (The critical
 * CSS that ps_html_process inlines is instead escaped: each such `<` in a CSS
 * string or comment is written as `\3c `.) */
PS_EXPORT PS_NODISCARD ps_error_t ps_css_validate(const char* css,
                                                  size_t css_len);

typedef const char* (*ps_css_lookup_fn)(const char* url, size_t* out_len,
                                        void* user_data);

/* Flatten @import statements by inlining imported CSS via `lookup`.
 * All-or-nothing per stylesheet: when any import cannot be inlined,
 * *out_css is the ORIGINAL input bytes (imports intact), *out_resolved
 * is 0, and *out_unresolved is a bail-fast sentinel (>= 1 on skip, NOT
 * an exhaustive count of unresolvable imports). On success
 * *out_unresolved is 0. */
PS_EXPORT PS_NODISCARD ps_error_t ps_css_flatten_imports(
    const char* css, size_t css_len, const char* css_url,
    ps_css_lookup_fn lookup, void* user_data, int max_depth, char** out_css,
    size_t* out_len, int* out_resolved, int* out_unresolved);

PS_EXPORT PS_NODISCARD ps_error_t ps_css_minify(const char* css, size_t css_len,
                                                char** out_css,
                                                size_t* out_len);

PS_EXPORT void ps_free(void* ptr);

/* ---------- HTML transform ---------- */

typedef struct ps_html_transform ps_html_transform_t;

/* Build a transform from a caller-supplied critical CSS block.  The page it
 * runs on is read and written as text/html; XHTML and other XML documents
 * are not supported.
 *
 * config->enable_async_css is IGNORED here: this entry point has no view of
 * the page's stylesheets and no browser, so it cannot establish that deferring
 * them is safe, and unsafe deferral is a flash of unstyled content. Use
 * ps_html_process for stylesheet deferral — it gathers the stylesheets and
 * gates on them. */
PS_EXPORT PS_NODISCARD ps_error_t ps_html_transform_create(
    const ps_scan_result_t* scan_result, const ps_html_config_t* config,
    const char* critical_css, size_t critical_css_len, ps_cache_t* cache,
    const char* hostname, const char* speculation_urls,
    size_t speculation_urls_len, ps_html_transform_t** out);

PS_EXPORT PS_NODISCARD ps_error_t ps_html_transform_run(
    ps_html_transform_t* transform, const char* html, size_t html_len,
    const char* url, char** out_html, size_t* out_len);

PS_EXPORT int ps_html_transform_modified(const ps_html_transform_t* transform);

PS_EXPORT void ps_html_transform_free(ps_html_transform_t* transform);

#ifdef __cplusplus
}
#endif

#endif /* PAGESPEED_H_ */
