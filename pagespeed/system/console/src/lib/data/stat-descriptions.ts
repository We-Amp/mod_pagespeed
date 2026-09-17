// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Operator-facing explanations of the statistics counters.
 *
 * The statistics registry carries counter *names* only -- there is no place in
 * the dump to hang a description on -- so this table is curated here in the
 * console. It has three layers, consulted in order:
 *
 *   1. `STAT_DESCRIPTIONS` -- an exact entry for a specific counter.
 *   2. `STAT_FAMILY_DESCRIPTIONS` -- a prefix entry describing a whole family,
 *      so a counter added to a known family still gets a useful explanation
 *      before anyone edits this file. The longest matching prefix wins.
 *   3. `NEUTRAL_STAT_DESCRIPTION` -- an honest "not described yet".
 *
 * The accompanying test asserts that every counter emitted by a real server
 * resolves at least to a family description, so the table cannot silently rot
 * as counters are added.
 *
 * Style rules for the text, enforced by the test:
 *   - Prose an operator can act on: what is counted, when it moves, and where
 *     useful what a non-zero value suggests. Not code paths.
 *   - No underscores. The search box matches descriptions as well as names, so
 *     a counter name quoted in the prose would make a name search match
 *     unrelated rows.
 */

/** Shown when neither an exact nor a family entry matches. */
export const NEUTRAL_STAT_DESCRIPTION =
  "No description available yet for this counter.";

/** Where a counter's description came from. */
export type StatDescriptionSource = "exact" | "family" | "none";

/**
 * Exact, per-counter descriptions.
 *
 * Every entry here was written from the registration and increment sites in
 * the server source. A counter whose meaning could not be established from
 * the source is deliberately left out, so it falls through to its family
 * description rather than being described inaccurately.
 */
export const STAT_DESCRIPTIONS: Readonly<Record<string, string>> = {
  // ---------------------------------------------------------------------
  // In-place optimization (ipro)
  // ---------------------------------------------------------------------
  ipro_served:
    "In-place requests answered under the original URL. It counts the serve, not the improvement: an unoptimized original served in place, and a not-modified response, are both counted here too.",
  ipro_not_in_cache:
    "In-place requests that found nothing cached for the URL, so the origin response is fetched and recorded for next time. Steady growth means cold or churning content.",
  ipro_not_rewritable:
    "In-place requests the server could not optimize, either because the URL is already known to be unoptimizable or because the lookup failed; the origin bytes are served unchanged.",

  ipro_daemon_served:
    "In-place requests answered from the optimizer daemon's shared cache, including not-modified revalidations. This is the daemon's hit count.",
  ipro_daemon_fallthrough:
    "In-place requests the daemon declined (nothing cached, or only a stale variant) or could not be asked because this process had no usable handle on its cache volume; the ordinary path serves them.",
  ipro_daemon_refresh_notified:
    "Refresh signals sent to the optimizer daemon after it offered a variant that was past its freshness lifetime, asking it to discard and rebuild that variant set.",
  ipro_daemon_refresh_notify_failed:
    "Refresh signals that could not be delivered to the optimizer daemon. Sustained growth points at a broken notification socket, leaving stale variants unhealed.",
  ipro_daemon_fallback_notified:
    "Notifications sent after serving a near-match variant, asking the daemon to build the exact variant this client asked for (viewport, pixel density, data saving, image format).",
  ipro_daemon_fallback_notify_failed:
    "Variant-request notifications that failed to send. Non-zero while near-match serves continue means variant sets will not converge; check the daemon notification socket.",

  ipro_recorder_resources:
    "In-place recordings started, that is origin responses the server began capturing for future optimization. Each finished recording also lands in exactly one outcome counter, so the outcomes trail this one by whatever is still in flight.",
  ipro_recorder_inserted_into_cache:
    "Recordings that completed and stored the resource, so later requests for that URL can be optimized and served in place.",
  ipro_recorder_not_cacheable:
    "Recordings abandoned because the response's caching headers forbid storing it. Expected for private responses; a large share means little content is eligible for in-place optimization.",
  ipro_recorder_failed:
    "Recordings that ended without a complete, storable response: a truncated response, or a write or decompression error. A visitor who navigates away mid-download also lands here, so a slow trickle is normal.",
  ipro_recorder_dropped_due_to_load:
    "Recordings abandoned at once because the configured limit on concurrent recordings was already reached. Sustained growth suggests raising that limit.",
  ipro_recorder_dropped_due_to_size:
    "Recordings abandoned because the response was larger than the configured maximum recordable size. The URL is remembered as not cacheable for a while.",
  ipro_recorder_dropped_content_type:
    "Recordings abandoned because the response was not an image, stylesheet or script. Normal for HTML, PDFs, fonts and plain text passing through.",
  ipro_recorder_empty:
    "Recordings abandoned because the origin returned a valid but zero-byte success response. The skip is remembered so the empty resource is not retried constantly.",
  ipro_recorder_error_status:
    "Recordings abandoned because the origin returned a client or server error for the resource. Non-zero points at broken or missing assets referenced by your pages.",
  ipro_recorder_skipped_transient:
    "Recordings skipped because the origin returned a status that may soon become a success, such as not-modified or partial content. Nothing is remembered, so the next request retries.",

  in_place_oversized_opt_stream:
    "In-place responses that grew past what can be cached while being transferred, forcing the server to stop buffering and stream the original bytes to the client instead.",
  in_place_uncacheable_rewrites:
    "In-place optimizations allowed on a response that was not cacheable, or had already expired. It requires two options together: optimizing uncacheable resources, and waiting in place for the optimized result.",

  // ---------------------------------------------------------------------
  // Zero-copy serving
  // ---------------------------------------------------------------------
  zerocopy_serve_aliased:
    "Cached optimized responses sent straight out of the shared memory region instead of being copied first, counted once per serve. Which requests qualify differs by server, so compare it against the other counters in this family rather than against total traffic.",
  zerocopy_serve_copied_out:
    "Serves that copied the body rather than sending it in place. Structural on nginx (compression, HTTP/2 or 3, ranges, subrequests) and on IIS (always the last block of an aliased serve), so a near-total count is normal on both. Degradation only on Apache.",
  zerocopy_serve_renew_fail_reset:
    "Serves reset because the shared region was overwritten while the response was still being sent, so no correct body could be delivered. Any sustained rate warrants cache tuning.",
  zerocopy_serve_ineligible:
    "On Apache, serves that could not send in place at all: any output filter outside a short allowlist -- TLS and HTTP/2 are outside it, so an HTTPS site always lands here -- plus buffered fetches, non-200 responses, subrequests, header-only and range requests.",
  zerocopy_serve_aborted:
    "On IIS, serves abandoned for reasons other than an overwritten region, namely memory allocation or submission failures. Non-zero indicates resource pressure in the serving path. Always zero on other servers.",
  zerocopy_serve_ring_refills:
    "On nginx, body windows handed to the client while streaming a large cached response over HTTP/2 or HTTP/3, counted once per window rather than once per response. Normal activity. Always zero on other servers, and on a site with no HTTP/2 or HTTP/3 traffic.",

  // ---------------------------------------------------------------------
  // Serving stale content, and error responses
  // ---------------------------------------------------------------------
  num_fallback_responses_served:
    "Times a stale cached copy was served because the origin fetch returned an error, keeping the site up on stale content. Non-zero means the origin is failing for those resources.",
  num_fallback_responses_served_while_revalidate:
    "Times a stale cached response was served immediately while a fresh copy was fetched in the background. Expected traffic when serving stale while revalidating is enabled.",
  slurp_404_count:
    "Requests in record-and-replay proxy mode that could not be satisfied and were answered with a not-found. Only meaningful on test or capture setups.",
  resource_404_count:
    "Not-found responses returned for optimized resource URLs. A steadily rising value suggests stale optimized URLs in upstream caches, or a configuration mismatch between servers.",

  // ---------------------------------------------------------------------
  // Reuse of optimization results, and the optimization workers
  // ---------------------------------------------------------------------
  rewrite_cached_output_hits:
    "Optimizations that finished before the page had to be flushed, so their result could be applied to that page. A deadline outcome, not a cache outcome.",
  rewrite_cached_output_misses:
    "Lookups for a stored optimization decision that found nothing usable, so the work had to be redone. Sustained high values mean a cold, undersized or churning cache. Not comparable with the hits counter, which measures a deadline instead.",
  rewrite_cached_output_missed_deadline:
    "Optimizations still unfinished when the page had to be flushed, so the original content went out and the result was kept for next time. Individual requests can ask to wait for every optimization instead, and those never land here.",
  num_rewrites_executed:
    "Optimization jobs actually started on a background worker. A rough measure of optimization work performed rather than reused from cache.",
  num_rewrites_dropped:
    "Optimization jobs cancelled before running because the server was shedding load or shutting down. Persistent growth means workers are saturated and pages go out unoptimized.",
  num_rewrites_abandoned_for_lock_contention:
    "Optimizations dropped immediately, without waiting, because the exclusive lock for that resource could not be taken, or because the server was shutting down. The lock case means another worker is already doing that same work.",
  total_rewrite_count:
    "HTML documents fully processed by the optimizer, counted once per document when parsing finishes. Effectively the volume of pages passing through optimization.",
  num_flushes:
    "Flush points reached while parsing HTML, where buffered output had to be sent onward. Frequent flushes shrink the window available for optimizing, so they can reduce coverage.",
  num_conditional_refreshes:
    "Revalidations where the origin answered not-modified and the stored copy was reused. High values are good: content is confirmed fresh without downloading it again.",
  num_proactively_freshen_user_facing_request:
    "Background refreshes started because a served cached resource was close to expiring. Non-zero means proactive freshening is active and hiding refetch latency from visitors.",
  num_resource_fetch_successes:
    "Fetches of original resources from the origin or upstream that returned content usable for optimization.",
  num_resource_fetch_failures:
    "Fetches of original resources that failed or returned unusable content. A rising value means resources cannot be retrieved, and so cannot be optimized.",
  resource_fetch_construct_successes:
    "Requests for an optimized resource URL that were successfully rebuilt on demand. Cheap resources are always rebuilt rather than looked up, so this counts more than just cache misses.",
  resource_fetch_construct_failures:
    "Requests for an optimized resource URL that could not be rebuilt, including jobs that could not be queued. Sustained values mean broken or expired optimized URLs are erroring.",
  resource_fetches_cached:
    "Requests for an optimized resource answered straight from cache with no reconstruction work. The desired outcome for optimized asset traffic.",
  resource_url_domain_acceptances:
    "Referenced URLs the server was permitted to optimize under its domain authorization rules.",
  resource_url_domain_rejections:
    "Referenced URLs left untouched because their domain is not authorized for optimization. High values usually mean missing authorization entries for a CDN or third-party host.",
  num_cache_control_rewritable_resources:
    "Fetched resources whose caching headers and content made them safe to optimize.",
  num_cache_control_not_rewritable_resources:
    "Resources judged unsafe to optimize for any reason: not cacheable, expired, marked no-transform, empty, but also a failed fetch, a client or server error, or a fetch shed under load. One resource can be counted once per filter.",
  not_cacheable:
    "Resources that cache extension refused because their response headers are not proxy-cacheable. High values mean origin caching headers are blocking longer asset lifetimes.",
  cache_extensions:
    "Resource references rewritten to a long-lived cacheable URL, counted when the change is applied to the page.",
  domain_rewrites:
    "URLs in the page rewritten to a different domain by domain mapping or sharding rules.",
  url_trims:
    "URLs in the page shortened by removing the part that is redundant with the page's own address.",
  url_trim_saved_bytes:
    "Total bytes of HTML saved by shortening those URLs. A direct measure of the payload reduction that URL trimming delivers.",
  converted_meta_tags:
    "HTML meta tags whose equivalent was copied into the real response headers, letting caches and clients act on them without parsing the page.",
  csp_blocked_rewrites:
    "Times a content security policy blocked loading a resource, using an optimized URL, or inlining an image as a data URI. Other refusals under the same policy are silent, so zero is not proof the policy costs you nothing.",
  num_css_inlined:
    "Stylesheet references replaced by their content directly in the page, removing a separate request.",
  num_js_inlined:
    "Script references replaced by their content directly in the page, counted only when the script really was inlined.",
  page_load_count:
    "Page load reports received from the browser instrumentation beacon. The denominator for the reported average load time.",
  total_page_load_ms:
    "Sum of the load times, in milliseconds, from those beacon reports. Divide by the page load count for the average; the two are not updated as one atomic step.",
  statistics_404_count:
    "Not-found responses accounted to the admin console's statistics endpoints. Nothing in this release ever records a value for it, so a flat zero is expected.",

  // ---------------------------------------------------------------------
  // The HTTP cache
  // ---------------------------------------------------------------------
  cache_hits:
    "HTTP cache lookups that returned a usable, still-fresh response. Rising with traffic is healthy; a low share against misses means most requests do origin work.",
  cache_misses:
    "HTTP cache lookups that did not yield a usable response, whether absent, expired or rejected by validity checks. Persistently high means poor reuse and more origin fetches.",
  cache_backend_hits:
    "Lookups where the cache backend actually returned stored bytes, before freshness was judged. Compare with the hit count to see how much stored content is being discarded as stale.",
  cache_backend_misses:
    "Lookups where the cache backend had nothing stored at all. High values point to an undersized, cold or failing cache backend.",
  cache_fallbacks:
    "Misses where a stale copy was nevertheless available and could be used as a fallback. Non-zero means content is expiring faster than it is being refreshed.",
  cache_expirations:
    "Misses caused specifically by the stored entry having passed its expiry. A high share of all misses suggests short cache lifetimes on your origin responses.",
  cache_inserts:
    "Responses written into the HTTP cache after passing cacheability checks. Near zero alongside heavy traffic means responses are being judged uncacheable.",
  cache_deletes:
    "Explicit removals of entries from the HTTP cache. Normally low; a spike indicates invalidation activity.",
  cache_time_us:
    "Total microseconds spent in HTTP cache operations. Dominated by lookups -- it is added to once per cache level per lookup, and again on each write -- so a rising total tracks cache latency, not write volume.",
  cache_flush_count:
    "Cache flushes observed and applied. Not one per flush: a purge is counted once per configured site and once per worker process that applies it, so a single purge can move this by a large step.",
  cache_flush_timestamp_ms:
    "When the most recently applied cache flush took effect, in milliseconds since 1970. A value, not a rate.",

  cache_batcher_coalesced_gets:
    "Lookups merged into an already in-flight request for the same key instead of hitting the cache again. High values mean the batcher is effectively suppressing duplicate work.",
  cache_batcher_queued_gets:
    "Lookups held in a queue because the parallel-lookup limit was already reached. Steady growth means the external cache is not keeping up with request concurrency.",
  cache_batcher_dropped_gets:
    "Lookups abandoned and answered as not-found because both the in-flight and queue limits were full. Any non-zero value means capacity limits are costing you hits under load.",

  compressed_cache_original_size:
    "Total bytes offered for storage before compression. The ratio to the compressed size is your effective compression rate.",
  compressed_cache_compressed_size:
    "Total bytes actually stored after compression. Compare against the original size to judge how much space compression is saving.",
  compressed_cache_corrupt_payloads:
    "Stored values that could not be decompressed and were treated as misses. Any non-zero value indicates corruption or truncation in the cache backend.",

  file_cache_hits:
    "Lookups in the main cache volume that found the key, whether the value came from its memory tier or from storage. Servers configured with several cache locations all report into this one counter.",
  file_cache_misses:
    "Main-volume lookups that did not find the key. A high share means the cache is cold, too small, or evicting aggressively.",
  file_cache_inserts:
    "Writes handed to the main cache volume. Counted as the write is submitted, so a write the cache then rejects is still counted here.",
  file_cache_deletes:
    "Deletions handed to the main cache volume, counted as the deletion is submitted whether or not the key was there.",
  file_cache_small_hits:
    "Hits in the small-object tier, a separate area for metadata and property entries so large-payload churn cannot evict them. The tier is on by default; when it cannot be carved out these lookups fall back to the main volume.",
  file_cache_small_misses:
    "Small-object tier lookups that found nothing.",
  file_cache_small_inserts: "Entries written into the small-object tier.",
  file_cache_small_deletes: "Entries explicitly removed from the small-object tier.",

  shm_cache_hits:
    "Shared-memory metadata cache lookups that found the key. A high hit share means worker processes are sharing optimization metadata well.",
  shm_cache_misses:
    "Shared-memory metadata cache lookups that found nothing. Persistently high suggests the shared segment is too small for your working set.",
  shm_cache_inserts:
    "Writes handed to the shared-memory cache, counted as submitted rather than as completed.",
  shm_cache_deletes:
    "Deletions handed to the shared-memory cache, counted as submitted whether or not the key was there.",

  cyclone_cache_hits:
    "Lookups the disk cache engine satisfied, from either its memory tier or disk. Equals the sum of the memory-tier and disk hit counters.",
  cyclone_cache_misses:
    "Lookups the disk cache engine actually performed and could not satisfy. Lookups made while the cache is shut down or was never started return empty without being counted here.",
  cyclone_cache_ram_hits:
    "Hits served from the engine's in-memory tier without touching disk. A high share of all hits means your hot set fits in memory.",
  cyclone_cache_disk_hits:
    "Hits that required reading the cache file. A high share against memory-tier hits suggests enlarging the memory tier.",
  cyclone_cache_inserts:
    "Values successfully written to the disk cache. Compare with the failure count to see whether writes are landing.",
  cyclone_cache_deletes:
    "Keys removed from the disk cache. Deleting a key that was not there still counts as a success.",
  cyclone_cache_failures:
    "Writes the disk cache engine rejected, including every write attempted while it is shut down or was never started. Any sustained non-zero value means an unwritable, unhealthy or absent cache.",
  cyclone_cache_bytes_read:
    "Total bytes returned by successful disk cache reads. Use it with the hit count to see average entry size and read volume.",
  cyclone_cache_bytes_written:
    "Total bytes accepted by successful disk cache writes. Rapid growth against a fixed cache size implies heavy eviction churn.",

  memcached_async_hits:
    "Hits on the asynchronous memcached view used by the request path.",
  memcached_async_misses:
    "Asynchronous memcached lookups that found nothing. A high share means poor reuse or an undersized memcached pool.",
  memcached_async_inserts:
    "Writes submitted to memcached through the asynchronous view.",
  memcached_async_deletes:
    "Deletions submitted to memcached through the asynchronous view.",
  memcached_blocking_hits:
    "Hits on the blocking memcached view, used where a synchronous answer is required. Usually far lower volume than the asynchronous view.",
  memcached_blocking_misses:
    "Blocking-view memcached lookups that found nothing.",
  memcached_blocking_inserts:
    "Writes submitted to memcached through the blocking view.",
  memcached_blocking_deletes:
    "Deletions submitted to memcached through the blocking view.",

  redis_async_hits: "Hits on the asynchronous Redis view used by the request path.",
  redis_async_misses:
    "Asynchronous Redis lookups that found nothing. A high share means poor reuse, eviction pressure, or a short configured entry lifetime.",
  redis_async_inserts: "Writes submitted to Redis through the asynchronous view.",
  redis_async_deletes: "Deletions submitted to Redis through the asynchronous view.",
  redis_blocking_hits:
    "Hits on the blocking Redis view, used where a synchronous answer is required.",
  redis_blocking_misses: "Blocking-view Redis lookups that found nothing.",
  redis_blocking_inserts: "Writes submitted to Redis through the blocking view.",
  redis_blocking_deletes: "Deletions submitted to Redis through the blocking view.",

  "pcache-cohorts-dom_hits":
    "Property-cache hits for the page-structure group. Higher is better: filters can reuse what earlier requests learned about the page.",
  "pcache-cohorts-dom_misses":
    "Page-structure lookups that found nothing, so that knowledge must be relearned. Expected on first views; persistently high means entries are being evicted.",
  "pcache-cohorts-dom_inserts":
    "Writes of page-structure properties back to the property cache.",
  "pcache-cohorts-dom_deletes":
    "Would count removals of page-structure properties, but the property cache has no delete operation, so it is always zero.",
  "pcache-cohorts-beacon_cohort_hits":
    "Property-cache hits for the measurement group, which holds what in-page beacons report about top-of-page images and style rules.",
  "pcache-cohorts-beacon_cohort_misses":
    "Measurement-group lookups that found nothing, so beacon-driven optimizations cannot apply yet. High values mean beacons are not coming back, or entries are evicted.",
  "pcache-cohorts-beacon_cohort_inserts":
    "Writes into the measurement group. Mostly ordinary page requests arming the next measurement rather than measurements coming back, so it tracks instrumented traffic more than beacon volume.",
  "pcache-cohorts-beacon_cohort_deletes":
    "Would count removals of measurement-group properties, but the property cache has no delete operation, so it is always zero.",
  "pcache-cohorts-dependencies_cohort_hits":
    "Property-cache hits for the dependency group, which records the resource dependencies discovered for a page. The group is only looked up when an enabled filter asks for it, so a stock configuration leaves this at zero.",
  "pcache-cohorts-dependencies_cohort_misses":
    "Dependency-group lookups that found nothing. Unless an enabled filter needs dependency data the group is skipped entirely rather than looked up and missed, so a stock configuration leaves this at zero.",
  "pcache-cohorts-dependencies_cohort_inserts":
    "Writes of dependency information into the property cache, made only when an enabled filter asked for the dependency group. Zero in a stock configuration.",
  "pcache-cohorts-dependencies_cohort_deletes":
    "Would count removals of dependency-group properties, but the property cache has no delete operation, so it is always zero.",

  memcache_timeouts:
    "Memcached operations that failed because the server did not answer in time. Any sustained non-zero value points to an overloaded server or too tight a timeout.",
  memcache_error_burst_size:
    "Errors counted in the current short error window. Reaching the threshold makes the server treat memcached as unhealthy and stop using it until the window clears.",
  memcache_last_error_checkpoint_ms:
    "When the current error-counting window started, in milliseconds since 1970. A marker, not a rate; a recent value means errors are ongoing.",

  redis_cluster_redirections:
    "Times a cluster node redirected an operation to a different node, forcing a retry. Steady growth means a stale slot map and avoidable extra round trips.",
  redis_cluster_slots_fetches:
    "Times the cluster slot-to-server map was fetched again. Frequent refetches indicate an unstable or resharding cluster.",

  purge_cancellations:
    "Pending purges abandoned because the shared purge lock could not be acquired in time. Non-zero means some purges silently did not take effect and need retrying.",
  purge_contentions:
    "Times writing or verifying the purge file failed, usually from lock contention or a permissions problem. Sustained non-zero values mean purges are unreliable.",
  purge_file_parse_failures:
    "Lines in the purge file that could not be parsed. Non-zero indicates a corrupt or hand-edited purge file.",
  purge_file_stats:
    "Times the purge file was read from disk. Grows steadily from routine polling; a sharp rise means frequent purge activity across worker processes.",
  purge_file_write_failures:
    "Purges that failed because the purge file could not be written or verified. Non-zero usually means a filesystem permissions or space problem.",
  purge_file_writes:
    "Times the purge file was rewritten, once per batch of applied purges.",
  purge_index:
    "A shared revision number bumped whenever the purge set changes, telling other worker processes to re-read the purge file. Not a count of purges.",

  downstream_cache_purge_attempts:
    "Purges the server decided to send to a caching layer in front of it, because it now has a better optimized version of a page. Zero means the feature is off or never triggers.",
  successful_downstream_cache_purges:
    "Purges to that downstream cache that were answered with success. A large gap below the attempt count means your proxy or CDN is rejecting them.",

  stdio_fs_outstanding_ops:
    "Filesystem operations in flight right now. This can fall as well as rise; a value stuck high means input and output are blocked or stalled.",
  stdio_fs_slow_ops:
    "Filesystem operations that took longer than the configured slow threshold; each also logs an error. Non-zero points to a slow or contended disk.",
  stdio_fs_total_ops:
    "Filesystem operations performed since startup, counted only while latency tracking is configured. A flat zero means tracking is off, not that there was no activity.",

  // ---------------------------------------------------------------------
  // Fetching from origins, queues and worker pools
  // ---------------------------------------------------------------------
  curl_fetch_request_count:
    "Outgoing fetches that ran to completion, counted once per finished fetch whether it succeeded or failed.",
  curl_fetch_bytes_count:
    "Total response body bytes received by completed outgoing fetches. Use it with the request count to gauge average origin response size and upstream bandwidth.",
  curl_fetch_time_duration_ms:
    "Total milliseconds spent on completed outgoing fetches. Divide by the request count for average origin latency; it grows faster than requests when origins slow down.",
  curl_fetch_cancel_count:
    "Would count fetches abandoned by an explicit bulk cancel, but nothing in this release performs one: shutdown finishes each in-flight fetch instead, so a flat zero is expected.",
  curl_fetch_active_count:
    "Outgoing fetches in flight right now. A persistently high value means fetches are piling up against a slow or unresponsive origin.",
  curl_fetch_timeout_count:
    "Outgoing fetches that hit the configured fetch timeout. A rising value points at an origin that is too slow or unreachable, or a timeout set too tight.",
  curl_fetch_failure_count:
    "Outgoing fetches that ended in a transport-level failure, such as a connection error, a timeout, or a secure request refused because secure fetching is disabled.",
  curl_fetch_cert_errors:
    "Fetches that failed on a certificate problem: untrusted, expired or unverifiable. Non-zero means an origin's certificate or your trust store needs attention.",
  curl_fetch_ultimate_success:
    "Fetches that completed and returned a usable final status. This is the usable-response count, as opposed to merely completed fetches.",
  curl_fetch_ultimate_failure:
    "Fetches with no usable outcome: a transport failure, a timeout, or a completed fetch whose status was an error. Client cancellations are excluded.",
  curl_fetch_last_check_timestamp_ms:
    "A timestamp belonging to the fetch counters. Nothing in this release ever records a value for it, so a flat zero is expected.",

  http_fetches:
    "Fetches that finished, counted for this site alone. Per-site counting is on by default on nginx and IIS and off by default on Apache; where it is off, this stays at zero.",
  http_bytes_fetched:
    "Total response body bytes received on the wire by those fetches, counted before any decompression. A compressed origin response is counted at its compressed size, not its expanded size.",
  http_approx_header_bytes_fetched:
    "Estimated total size of the response headers on those fetches. Useful next to the body byte count to spot origins returning unusually heavy headers.",

  total_fetch_count:
    "Completed requests for optimized resource URLs, whether the fetch succeeded or failed. This tracks how much of your traffic is optimized-asset serving.",
  "dropped-fetch-count":
    "Background fetches shed because the per-host or global fetch queue was already full. Sustained growth means background optimization is being throttled by a slow origin.",
  "queued-fetch-count":
    "Background fetches deferred into a per-host queue because that host already had its maximum outstanding requests. Growth means rate limiting is actively kicking in.",
  "current-fetch-queue-size":
    "Background fetches sitting in the deferred queues across all hosts right now. Staying near the configured maximum means fetches are about to start being dropped.",
  "current-expensive-operations":
    "Costly optimization operations running concurrently right now. It is capped at a configured bound; sitting at the cap means new optimizations are being turned away.",

  url_input_resource_hit:
    "Times a resource needed as input to an optimization was found in the HTTP cache, so no origin fetch was required. Higher is better.",
  url_input_resource_miss:
    "Times an input resource was not cached and had to be fetched from the origin and stored. A high ratio against hits suggests short cache lifetimes or too small a cache.",
  url_input_resource_recent_fetch_failure:
    "Times an input resource was skipped because a recent fetch failure for that URL was still remembered. Persistent growth points at a broken or unreachable resource URL.",
  url_input_resource_recent_uncacheable_failure:
    "Times an input resource was skipped because it was recently found to be uncacheable, and the configuration does not allow optimizing uncacheable resources.",
  url_input_resource_recent_uncacheable_miss:
    "Times a remembered uncacheable or load-shed result was deliberately ignored and the resource fetched anyway, because the request was user-facing or the option allows it.",

  font_service_input_resource_hit:
    "Cache hits for a stylesheet fetched from the hosted font service, avoiding a fetch. Tracked separately because those responses vary by browser.",
  font_service_input_resource_miss:
    "Font service stylesheets that were not cached and had to be fetched.",
  font_service_input_resource_recent_fetch_failure:
    "Times a font service resource was skipped because a recent fetch failure for it was still remembered. Growth suggests the font service is unreachable.",
  font_service_input_resource_recent_uncacheable_failure:
    "Times a font service resource was skipped because it was recently found uncacheable and uncacheable resources are not being optimized.",
  font_service_input_resource_recent_uncacheable_miss:
    "Times a remembered uncacheable or load-shed font service result was ignored and the resource fetched again, typically because the request was user-facing.",

  "html-worker-queue-depth":
    "Would report the depth of the HTML processing worker queue, but this release never records a value for worker queue depths, so a flat zero is expected.",
  "rewrite-worker-queue-depth":
    "Would report the depth of the main optimization worker queue, but this release never records a value for worker queue depths, so a flat zero is expected.",
  "low-priority-worked-queue-depth":
    "Would report the depth of the low-priority worker queue, but this release never records a value for worker queue depths, so a flat zero is expected. That queue also has no length limit, so nothing is shed from it.",

  "named-lock-rewrite-scheduler-granted":
    "Times an optimization obtained the exclusive lock for its cache key and was allowed to proceed. Effectively the number of optimizations started under the scheduler.",
  "named-lock-rewrite-scheduler-denied":
    "Times an optimization could not get its lock because another worker held it, so that attempt was abandoned. High values mean duplicate work is being suppressed, usually healthy.",
  "named-lock-rewrite-scheduler-locks-held":
    "Optimization locks held right now. It should track your concurrency and fall back toward zero when idle; a stuck non-zero value hints at stalled optimizations.",
  "named-lock-rewrite-scheduler-released-not-held":
    "Times a finishing optimization tried to release a lock it no longer owned, normally because the lock had been stolen after running past the steal deadline. Small numbers are benign.",
  "named-lock-rewrite-scheduler-stolen":
    "Times an optimization lock was taken from a holder that kept it past the steal timeout. Steady growth suggests optimizations outlast the steal window, causing redundant work.",

  num_deadline_alarm_invocations:
    "Times an optimized-resource fetch missed its deadline and was detached, so the original content was served while optimization continued in the background.",
  child_shutdown_count:
    "Times a worker process shut the optimization module down in an orderly way. Restarts and reloads move it, and so does routine worker recycling; a crashing worker never gets to increment it.",
  beacon_overflow_count:
    "Browser measurement reports that arrived flagged as truncated because the payload exceeded its size budget. A high value means some measurement data is being lost.",
  timestamp_:
    "When the most recent statistics snapshot was written to the console log, in milliseconds since 1970. Present only when statistics logging to a file is enabled.",
  _purge_poll_timestamp_ms:
    "When the purge file was last polled, in milliseconds since 1970. Internal bookkeeping used to pace polling, not a health signal.",

  web_bot_auth_verified_signed_requests:
    "On nginx, requests whose signature verified against a published key: either a confirmed bot or a signed agent. Needs both the classification option and its separate counting option turned on, and both default to off.",
  web_bot_auth_other_signature_requests:
    "On nginx, requests that carried signature material from a different signing scheme rather than the bot one. They are treated as unsigned; the counter keeps them visible instead of silently ignored.",

  "proxy-all-mode-all-requests":
    "Requests handled while the server acts as a full proxy for another origin. The denominator for the other counters in this family.",
  "proxy-all-mode-pagespeed-requests":
    "Proxied requests recognised as optimized-resource URLs and served straight from the optimization pipeline rather than passed through to the origin.",
  "proxy-all-mode-publisher-rejected-requests":
    "Proxied requests refused by configuration because the URL or its headers matched a decline rule. The client gets a short blocked-by-the-administrator page.",
  "proxy-all-mode-without-domain-config-requests":
    "Proxied requests for which no domain-specific configuration was found, so defaults were used. Large numbers usually mean a domain is missing from your proxy configuration.",
  "proxy-all-mode-without-domain-config-resource-requests":
    "The subset of those requests that were for optimized resources rather than pages. Useful for telling whether missing domain configuration is affecting assets specifically.",

  // ---------------------------------------------------------------------
  // Image optimization
  // ---------------------------------------------------------------------
  image_rewrites:
    "Images successfully re-encoded and written as an optimized derivative. Each one is a source image that got smaller; rising with traffic is normal and healthy.",
  image_rewrite_uses:
    "Times an optimized image URL was substituted into a page or stylesheet. It counts deliveries, not optimizations, so it grows much faster than the rewrite count on cached content.",
  image_inline:
    "Times an image was small enough to be embedded directly into the page or stylesheet, removing a request. Counted once per element rewritten.",
  image_webp_rewrites:
    "Optimized images whose output format was WebP. Near zero means clients are not being offered WebP, or the sources are unsuitable.",
  image_avif_rewrites:
    "Optimized images whose output format was AVIF. AVIF is the most compact but the most expensive to encode, so weigh this against the AVIF timeout and overrun counters.",
  image_ongoing_rewrites:
    "Image optimizations running right now. It goes up and down; a value pinned at the configured concurrency limit means image work is saturated.",
  image_file_count_reduction:
    "Image requests eliminated by combining several background images into one sprite. Higher is better.",
  image_rewrite_total_bytes_saved:
    "Cumulative bytes removed from images that were successfully optimized, counting only rewrites that actually shrank. Pair it with the original-bytes counter for an average saving.",
  image_rewrite_total_original_bytes:
    "Cumulative original size of all successfully optimized images. Divide the bytes saved by this for the overall image compression ratio.",
  image_rewrite_latency_total_ms:
    "Total milliseconds spent inside image optimization, successful and failed alike. Divide by the number of attempts for the average cost per image.",
  image_norewrites_high_resolution:
    "Images left untouched because their pixel dimensions exceeded the configured resolution limit, or could not be read as a positive width and height at all. The second case points at corrupt image headers.",
  image_resized_using_rendered_dimensions:
    "Images resized to the dimensions a real browser actually rendered, as reported by the measurement beacon, rather than to the dimensions in the markup. Requires beaconing.",
  image_rewrites_squashing_for_mobile_screen:
    "Would count images shrunk to suit a small mobile screen. Nothing in this release ever records a value for it, so a flat zero is expected.",
  image_rewrites_dropped_decode_failure:
    "Image optimizations abandoned because the optimized URL could not be decoded. Steady growth suggests malformed or tampered optimized URLs.",
  image_rewrites_dropped_due_to_load:
    "Image optimizations cancelled before starting because the server was too busy. Sustained non-zero means image work is being shed under load; consider more capacity.",
  image_rewrites_dropped_intentionally:
    "Image optimizations that ran and deliberately produced no output, for any of the specific reasons below. Work shed because the server was too busy is counted separately and is not included here.",
  image_rewrites_dropped_mime_type_unknown:
    "Images skipped because the real format could not be identified from the file's leading bytes. Points at corrupt files, or non-images served under image URLs.",
  image_rewrites_dropped_nosaving_noresize:
    "Recompressed images discarded because the new version was no smaller. Common and harmless on an already optimized image library.",
  image_rewrites_dropped_nosaving_resize:
    "Resized images discarded because the resized version still saved no space. Harmless; the original is served unchanged.",
  image_rewrites_dropped_server_write_fail:
    "Optimized image content that could not be written to the cache or filesystem. Non-zero warrants checking disk space, permissions and cache health.",
  image_webp_alpha_timeouts:
    "WebP encodes of images with transparency abandoned on the conversion timeout, producing nothing. Persistent values mean the WebP time budget is too tight.",
  image_webp_opaque_timeouts:
    "WebP encodes of fully opaque images that timed out and produced no WebP. Another format is used instead, which for most sources is still an optimized image rather than the untouched original.",
  image_webp_conversion_gif_timeouts:
    "Conversions from GIF to WebP abandoned on the timeout with no WebP produced. By default the image still ships optimized, as a PNG, rather than falling all the way back to the original GIF.",
  image_webp_conversion_gif_animated_timeouts:
    "Conversions from animated GIF to WebP abandoned on the timeout. Animated conversions are the most expensive, so this is the first to move under a tight budget.",
  image_webp_conversion_jpeg_timeouts:
    "Conversions from JPEG to WebP abandoned on the timeout with no output produced.",
  image_webp_conversion_png_timeouts:
    "Conversions from PNG to WebP abandoned on the timeout with no output produced.",
  image_avif_conversion_avif_timeouts:
    "Re-encodes of images that were already AVIF, abandoned because the estimated cost did not fit the AVIF budget, so no new AVIF was produced.",
  image_avif_conversion_avif_overruns:
    "Re-encodes of already-AVIF images that did produce output but ran past the AVIF budget. The result was kept; the cost estimate is running low for this traffic.",
  image_avif_conversion_gif_animated_timeouts:
    "Conversions from GIF to AVIF that produced nothing against the AVIF time budget: refused before starting because the estimated cost did not fit, or abandoned partway once it ran over.",
  image_avif_conversion_gif_animated_overruns:
    "GIF-to-AVIF encodes that were kept but ran past the AVIF budget. Only still GIFs can land here: an animated encode is interrupted instead, and counted as a timeout.",
  image_avif_conversion_jpeg_timeouts:
    "Conversions from JPEG to AVIF that produced no output because the AVIF budget was exceeded or the cost estimate did not fit.",
  image_avif_conversion_jpeg_overruns:
    "Conversions from JPEG to AVIF whose result was kept but which exceeded the AVIF budget. Non-zero means AVIF is costing more latency than configured.",
  image_avif_conversion_png_timeouts:
    "Conversions from PNG to AVIF that produced no output because the AVIF budget was exceeded or the cost estimate did not fit.",
  image_avif_conversion_png_overruns:
    "Conversions from PNG to AVIF whose result was kept but which exceeded the AVIF budget.",

  // ---------------------------------------------------------------------
  // Stylesheet optimization
  // ---------------------------------------------------------------------
  css_filter_uses:
    "Times an optimized stylesheet was actually used on a page, external or inline. It counts deliveries rather than optimizations, so it grows with traffic once the cache is warm.",
  css_filter_blocks_rewritten:
    "Stylesheets successfully parsed and rewritten, each now shipping in optimized form. Usually smaller, but not always: an already optimized stylesheet can be rewritten without shrinking.",
  css_filter_parse_failures:
    "Stylesheets the parser could not read cleanly. Non-zero means those files fall back to a limited URL-only rewrite; the log names the offending files.",
  css_filter_rewrites_dropped:
    "Stylesheets discarded because the optimized version was no smaller than the original. Common on already minified stylesheets, and harmless.",
  css_filter_fallback_rewrites:
    "Unparseable stylesheets that were still improved by rewriting the URLs inside them. A safety net that keeps some benefit when full optimization is impossible.",
  css_filter_fallback_failures:
    "Unparseable stylesheets where even the URL-only fallback failed, so the file is passed through untouched.",
  css_filter_total_bytes_saved:
    "Net bytes removed from rewritten stylesheets. A rewrite that grew a stylesheet subtracts from it, so unlike most counters this one can go down as well as up.",
  css_filter_total_original_bytes:
    "Cumulative original size of all successfully optimized stylesheets, the denominator for the stylesheet saving ratio.",
  css_file_count_reduction:
    "Stylesheet requests eliminated by combining several files into one. Directly reflects requests saved per page view.",
  css_combine_opportunities:
    "Stylesheet requests that could in principle be merged away, counted from the markup as the number of stylesheet links per page minus one. Compare with the reduction to see how much is realised.",
  css_elements_moved:
    "Style and stylesheet elements relocated into the head, or above scripts, so they stop blocking rendering later in the document.",
  css_imports_to_links:
    "Inline style blocks whose leading imports were converted into ordinary stylesheet links, so the other stylesheet filters can then optimize them.",
  flatten_imports_charset_mismatch:
    "Stylesheets not merged into their parent because their character encoding disagreed with the page's. Safe, but no flattening benefit.",
  flatten_imports_complex_queries:
    "Import flattening refused because a media query was more complex than a plain media type. Only simple media types can be merged safely.",
  flatten_imports_invalid_url:
    "Import flattening skipped because an import pointed at a URL that could not be resolved. Non-zero usually indicates broken references in the stylesheets.",
  flatten_imports_limit_exceeded:
    "Flattened stylesheets discarded because the merged result reached the configured size limit. Raise that limit to flatten large stylesheet trees.",
  flatten_imports_minify_failed:
    "Imports not merged for either of two reasons: the imported stylesheet could not be parsed, or the merged result could not be written back out in minified form. The original imports are left in place.",
  flatten_imports_recursion:
    "Imports refused because a stylesheet imports itself, directly or through a cycle. Non-zero points at a genuine loop in the site's stylesheets.",
  flatten_imports_unparseable_import:
    "Stylesheets rejected for flattening because an import survived parsing as an unrecognised block. Counted once per stylesheet.",

  // ---------------------------------------------------------------------
  // Script optimization
  // ---------------------------------------------------------------------
  javascript_blocks_minified:
    "Scripts the minifier processed successfully. This includes minifications that saved nothing; the reducing-minifications counter tracks only those that actually shrank.",
  javascript_minify_uses:
    "Times an optimized script was swapped into a page, inline or external. It counts deliveries rather than minifications, so it grows with traffic.",
  javascript_did_not_shrink:
    "Scripts left as they were because minification produced nothing usable -- either nothing smaller, or a failure to parse the script at all. Common and harmless on already minified libraries.",
  javascript_failed_to_write:
    "Minified scripts that could not be written to the cache. Non-zero warrants checking disk space, permissions and cache health.",
  javascript_libraries_identified:
    "Scripts recognised as known public libraries, so they can be redirected to a canonical copy instead of being optimized locally.",
  javascript_minification_disabled:
    "Scripts skipped because external script rewriting is not enabled in the active configuration. Non-zero is a configuration signal, not an error.",
  javascript_minification_failures:
    "Scripts the minifier could not process, usually because of a syntax error. The original code is preserved; the log names the files.",
  javascript_reducing_minifications:
    "Minifications that genuinely made a script smaller and will be used. The subset of minified blocks that produced real savings.",
  javascript_total_bytes_saved:
    "Cumulative bytes removed from scripts that actually shrank. Pair it with the original-bytes counter for the overall script compression ratio.",
  javascript_total_original_bytes:
    "Cumulative original size of the scripts that were successfully shrunk, the denominator for the script saving ratio.",
  js_file_count_reduction:
    "Script requests eliminated by combining several script files into one. Directly reflects requests saved per page view.",

  // ---------------------------------------------------------------------
  // Deferred loading and above-the-fold optimization
  // ---------------------------------------------------------------------
  lazyload_images_applied:
    "Images deferred using the script-based loader, with the real source moved aside and restored when the image nears the viewport.",
  lazyload_images_native_applied:
    "Images deferred using the browser's own lazy-loading attribute, with no injected script. Preferred over the script-based path where available.",
  lazyload_images_skipped_critical:
    "Images deliberately not deferred because they are known to be visible on first paint. Those get eager, high-priority loading instead.",
  lazyload_images_skipped_csp:
    "Pages where script-based deferred loading was suppressed because the content security policy forbids inline script. Counted at most once per page.",
  critical_css_beacon_filter_script_added_count:
    "Pages that received the measurement script reporting which style rules are used at the top of the page. Each page is re-measured only once per interval, so this stays well below the page view count.",
  critical_css_no_beacon_due_to_missing_data:
    "Would count pages where the measurement script was withheld for lack of stored data. Nothing in this release ever records a value for it, so a flat zero is expected.",
  critical_css_skipped_due_to_charset:
    "Would count stylesheets skipped because their character encoding did not match the page. Nothing in this release ever records a value for it, so a flat zero is expected.",
  critical_images_beacon_filter_script_added_count:
    "Pages that received the measurement script reporting which images are visible at the top of the page. Each page is re-measured only once per interval, so this stays well below the page view count.",
  critical_images_valid_count:
    "Times a usable, unexpired record of top-of-page images was found for a page. A healthy share of all lookups means the measurement data is working.",
  critical_images_expired_count:
    "Times the stored top-of-page image data existed but was too old to use. Persistently high means the data expires faster than visitors refresh it.",
  critical_images_not_found_count:
    "Times no usable top-of-page image data existed, so image prioritisation could not be applied. Normal for pages that have not been measured yet.",
  critical_selectors_valid_count:
    "Times usable, unexpired data about which style rules are needed at the top of the page was found, allowing the rest of the stylesheet to be deferred.",
  critical_selectors_expired_count:
    "Times the stored top-of-page style-rule data was too old to use. Persistently high means the data expires faster than visitors refresh it.",
  critical_selectors_not_found_count:
    "Times no top-of-page style-rule data existed for a page. Normal for pages that the measurement script has not yet covered.",
  num_css_used_for_critical_css_computation:
    "Stylesheets on a page that were successfully summarised and could contribute to working out which rules are needed at the top of the page.",
  num_css_not_used_for_critical_css_computation:
    "Stylesheets that could not be summarised, for example because the fetch failed or another filter removed them. A high share means that calculation works from an incomplete picture.",
  prioritize_critical_images_applied:
    "Images marked as high fetch priority because they are known to appear at the top of the page. Non-zero means the measurement data is being put to use.",

  // ---------------------------------------------------------------------
  // Injected scripts, ads, and client-side storage
  // ---------------------------------------------------------------------
  instrumentation_filter_script_added_count:
    "Pages that received the timing script which reports real page load measurements back to the server. It should closely track instrumented page views.",
  inserted_ga_snippets:
    "Pages where an analytics snippet was injected because the page did not already carry one.",
  show_ads_api_replaced_for_async:
    "Legacy ad interface calls replaced with the modern asynchronous equivalent, so the ad no longer blocks parsing.",
  show_ads_snippets_converted:
    "Legacy ad configuration snippets successfully rewritten into the modern asynchronous ad tag.",
  show_ads_snippets_not_converted:
    "Legacy ad snippets recognised but left untouched because they could not be converted safely. Non-zero means some ads still use the blocking form.",
  num_local_storage_cache_candidates_added:
    "Inlined resources marked as eligible to be kept in the browser's local storage for a later visit.",
  num_local_storage_cache_candidates_found:
    "Elements in the markup already marked as local-storage candidates, which are then checked against the visitor's stored copies.",
  num_local_storage_cache_candidates_removed:
    "Resources whose local-storage markers were stripped because they ended up not being inlined, and so cannot be stored.",
  num_local_storage_cache_stored_css:
    "Stylesheets replaced by a small script that restores them from the visitor's local storage instead of sending the styles again.",
  num_local_storage_cache_stored_images:
    "Inlined images replaced by a small script that restores them from the visitor's local storage instead of sending the image data again.",
  num_local_storage_cache_stored_total:
    "Resources served from the visitor's local storage rather than sent again: the sum of the stored stylesheets and images.",
  num_dedup_inlined_images_candidates_found:
    "Inlined images large enough to be worth de-duplicating that were examined on the page.",
  num_dedup_inlined_images_candidates_replaced:
    "Repeat copies of an already inlined image replaced by a short script referring to the first copy. Non-zero means real bytes saved on pages that reuse images.",
};

/**
 * Family descriptions, keyed by counter-name prefix. The longest matching
 * prefix wins, so a nested family can refine a broader one.
 */
export const STAT_FAMILY_DESCRIPTIONS: ReadonlyArray<readonly [string, string]> =
  [
    [
      "ipro_",
      "In-place optimization: requests for a stylesheet, script or image that are optimized and served under their original URL instead of a rewritten one. This family tracks whether such a request was answered from the optimized cache, missed it, or could not be optimized at all.",
    ],
    [
      "ipro_daemon_",
      "In-place optimization on servers where the optimized cache is owned by the optimizer daemon rather than by the serving module. These counters split daemon-eligible requests into served and declined, and count the notifications sent to make the daemon rebuild or add a variant.",
    ],
    [
      "ipro_recorder_",
      "The recorder that captures an origin response the first time an in-place URL misses the cache. Each finished recording is counted in exactly one outcome, so the outcomes trail the recordings started by whatever is in flight. Where the optimizer daemon owns the cache it records separately and moves none of these.",
    ],
    [
      "zerocopy_serve_",
      "Serving cached optimized bodies straight out of shared memory without copying them. Three counters exist on one server each and are always zero on the others: the window counter on nginx, the ineligible counter on Apache, the aborted counter on IIS. Read the rest per server rather than as one ratio.",
    ],
    [
      "in_place_",
      "In-place optimization of resources served under their original URL, covering cases where the response was too large to cache mid-transfer or was optimized despite not being cacheable.",
    ],

    [
      "cache_",
      "The HTTP cache: the store of cacheable responses, both fetched originals and optimized resources, layered over whichever cache backend you configured. Hits and misses are counted after freshness and validity checks.",
    ],
    [
      "cache_batcher_",
      "A batching layer in front of an external cache that bounds parallel lookups by merging duplicate keys and queueing the rest. It shows how much lookup traffic was merged, deferred or discarded under load.",
    ],
    [
      "compressed_cache_",
      "A compression layer that deflates values before storing them and inflates them on read. It reports bytes in against bytes out, and payloads that failed to inflate.",
    ],
    [
      "file_cache_",
      "The main cache volume, measured where the server hands work to it. That is the whole cache engine including its memory tier -- and, if the cache engine could not start, a purely in-memory cache instead, in which case nothing here is on disk at all. Writes and deletions are counted as submitted, not as completed.",
    ],
    [
      "shm_cache_",
      "The shared-memory cache: a fixed-size segment shared by all worker processes. Its traffic always mixes optimization metadata with a second user -- the property cache in a stock setup, or the file-metadata cache once an external cache is configured -- so it is never metadata alone.",
    ],
    [
      "cyclone_cache_",
      "The disk cache engine itself, with its own scan-resistant eviction, memory-mapped reads and optional memory tier. It reports its own hits, misses, inserts, deletes, tier split, read and write volume, and write failures.",
    ],
    [
      "memcached_",
      "The memcached backend, wrapped twice: an asynchronous view used by the request path and a blocking view used where a synchronous answer is required. Both report hits, misses, inserts and deletes for the same server pool.",
    ],
    [
      "memcache_",
      "The health of the memcached connection rather than its cache traffic: operation timeouts and the sliding error window that decides whether memcached is still considered usable.",
    ],
    [
      "redis_",
      "The Redis backend, wrapped twice as an asynchronous view for the request path and a blocking view for synchronous work, plus the cluster bookkeeping used when talking to a Redis cluster.",
    ],
    [
      "pcache-cohorts-",
      "The property cache, which stores per-URL knowledge learned from earlier requests. A cohort is one named group of properties read and written together, so you can see which group is paying off.",
    ],
    [
      "purge_",
      "The cache purge subsystem, which coordinates invalidation across worker processes through a shared purge file and a lock. It counts file reads, writes, failures, contention and abandoned purges.",
    ],
    [
      "downstream_cache_",
      "Purges the server sends to a caching layer in front of it, such as a proxy or CDN, so that a stale under-optimized page is replaced by the better version on the next request.",
    ],
    [
      "stdio_fs_",
      "Filesystem activity performed through the server's file layer, counted only while latency tracking is configured. It shows operations in flight, operations in total, and those over the slow threshold.",
    ],

    [
      "curl_fetch_",
      "The built-in HTTP client that fetches pages and resources from your origins. Together these show fetch volume, bytes, latency, and how the fetches ended: success, timeout, transport failure or certificate error.",
    ],
    [
      "web_bot_auth_",
      "On nginx only: verification of signed automated clients, split into signatures that verified against a published key and signature material belonging to some other scheme. Counting needs its own option on top of the classification option, and both default to off, so zero usually just means not enabled.",
    ],
    [
      "http_",
      "Per-site fetch counters: completed fetches, on-the-wire body bytes counted before any decompression, and estimated header bytes. Per-site counting is on by default on nginx and IIS and off by default on Apache, where these stay at zero.",
    ],
    [
      "url_input_resource_",
      "The cache outcome when an ordinary resource is needed as input to an optimization: hits and misses, plus three outcomes for a URL recently learned to be broken or uncacheable. Read them as a cache-effectiveness ratio.",
    ],
    [
      "font_service_input_resource_",
      "The same outcomes as the general input-resource family, but only for stylesheets fetched from the hosted font service, which is tracked separately because its responses vary by browser.",
    ],
    [
      "named-lock-",
      "The lock that stops several workers optimizing the same resource at once: locks granted, denied, currently held, stolen after a timeout, and released after being stolen. Heavy denial is normal deduplication; heavy stealing is not.",
    ],
    [
      "proxy-all-mode-",
      "Requests handled while the server proxies an entire external origin. They break proxied traffic down into optimized-resource requests, requests declined by configuration, and requests with no matching domain configuration.",
    ],

    [
      "image_",
      "Image optimization: recompression, format conversion, resizing, inlining and spriting. These counters cover successful rewrites, bytes saved, work in flight, and every reason a rewrite was abandoned.",
    ],
    [
      "image_rewrites_dropped_",
      "Reasons an image optimization ended without producing an optimized file. Most are benign, such as nothing to save or an unrecognised format; write failures and load shedding are the ones worth alerting on.",
    ],
    [
      "image_webp_",
      "WebP encoding outcomes, broken down by source format and by whether the source had transparency. The timeout counters are conversions that produced nothing because the time budget was hit.",
    ],
    [
      "image_avif_",
      "AVIF encoding outcomes by source format. A timeout means no AVIF was produced, so raising the budget would regain the optimization; an overrun means AVIF was produced and served but cost more time than budgeted.",
    ],
    [
      "css_",
      "Stylesheet handling as a whole: optimizing, combining, relocating to the head, and turning inline imports into links. Together they show how much stylesheet weight and how many stylesheet requests are being removed.",
    ],
    [
      "css_filter_",
      "The core stylesheet optimizer: parsing, minifying and rewriting the URLs inside stylesheets. It covers successes, bytes saved, parse failures, and the reduced fallback used when a stylesheet cannot be parsed.",
    ],
    [
      "flatten_imports_",
      "Reasons stylesheet imports could not be merged into their parent file. All are safe refusals that leave the styles working; a consistently high one identifies a fixable pattern in the site's stylesheets.",
    ],
    [
      "javascript_",
      "Script minification and library identification: scripts minified, bytes saved, scripts delivered in optimized form, and each reason minification was skipped or failed.",
    ],
    [
      "js_",
      "Script combining, which merges several script files into one to cut the request count. Distinct from the minification counters, which use the longer prefix.",
    ],
    [
      "lazyload_images_",
      "Deferred image loading, through either the browser's native attribute or an injected script. It also records images deliberately not deferred because they are at the top of the page or blocked by a content security policy.",
    ],
    [
      "critical_",
      "Optimization of what the browser needs to paint the top of the page: injecting a measurement script, storing what it reports, and using that to prioritise the visible images and style rules. It needs real browser traffic before it does anything.",
    ],
    [
      "critical_css_",
      "Measuring and using which style rules are needed to paint the top of the page, so the rest can be deferred. It depends on a measurement script running in real browsers.",
    ],
    [
      "critical_images_",
      "Availability of measured data about which images appear at the top of the page. The valid, expired and not-found split shows how healthy that measurement pipeline is.",
    ],
    [
      "critical_selectors_",
      "Availability of measured data about which style rules are needed at the top of the page, split into usable, too old, and never recorded. Stored data that fails to decode is counted in none of the three, so they need not add up to the lookups made.",
    ],
    [
      "show_ads_",
      "Conversion of legacy synchronous ad snippets into the modern asynchronous ad tag, so that ads stop blocking page parsing.",
    ],
    [
      "num_local_storage_cache_",
      "Keeping small inlined images and stylesheets in the visitor's browser storage, so a repeat page view can restore them instead of downloading them again.",
    ],
    [
      "num_dedup_inlined_images_",
      "Replacing repeated copies of the same inlined image on a page with a short reference to the first copy, removing duplicated data from the page.",
    ],
  ];

/** The description for a counter, always non-empty. */
export function describeStat(name: string): string {
  const exact = exactDescription(name);
  if (exact) return exact;

  const family = familyDescription(name);
  if (family) return family;

  return NEUTRAL_STAT_DESCRIPTION;
}

/** Which layer of the table answered for this counter. */
export function statDescriptionSource(name: string): StatDescriptionSource {
  if (exactDescription(name)) return "exact";
  if (familyDescription(name)) return "family";
  return "none";
}

/**
 * The exact entry for a counter, if the table really has one.
 *
 * Counter names come from the server, so they are arbitrary strings -- and a
 * plain object inherits `toString`, `constructor` and friends from its
 * prototype. Indexing without this guard would answer a stat literally named
 * "toString" with a function, which then renders as source code in the
 * tooltip. Only own properties count.
 */
function exactDescription(name: string): string | null {
  if (!Object.hasOwn(STAT_DESCRIPTIONS, name)) return null;
  return STAT_DESCRIPTIONS[name];
}

/** The longest family prefix that matches, if any. */
function familyDescription(name: string): string | null {
  let best: string | null = null;
  let bestLength = -1;
  for (const [prefix, text] of STAT_FAMILY_DESCRIPTIONS) {
    if (prefix.length > bestLength && name.startsWith(prefix)) {
      best = text;
      bestLength = prefix.length;
    }
  }
  return best;
}
