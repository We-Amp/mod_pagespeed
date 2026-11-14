// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// Read-only view of a cache entry. Abstracts ReadResult for testability.
/// </summary>
public interface IReadResult : IDisposable
{
    ReadOnlyMemory<byte> ContentMemory { get; }
    uint Mask { get; }
    PageSpeedContentType ContentType { get; }
    string? OriginContentType { get; }
    byte Flags { get; }
    bool NeedsRevalidation { get; }

    /// <summary>
    /// Origin Cache-Control directives as a PS_CC_ORIGIN_* bitfield.
    /// Exposed via ps_read_origin_cc_flags P/Invoke.
    /// </summary>
    ushort OriginCcFlags { get; }

    /// <summary>
    /// The origin's <c>s-maxage</c> in seconds as recorded with the entry
    /// (ps_read_origin_s_maxage); 0 when absent or not recorded.
    /// </summary>
    uint OriginSMaxage => 0;

    /// <summary>
    /// Whether the origin response requires revalidation before serving.
    /// Derived from OriginCcFlags (PS_CC_ORIGIN_* bitfield) via
    /// ps_read_origin_cc_flags. For a private cache (ASP.NET in-process),
    /// only no-cache and must-revalidate trigger revalidation; proxy-revalidate
    /// and s-maxage are ignored per RFC 9111 §5.2.2.9-10.
    ///
    /// NOTE: The flags byte bit 0x02 is C++ kFlagWorkerProcessed (worker
    /// dedup), not an origin-revalidation signal. Do not use that bit for
    /// revalidation logic.
    /// </summary>
    bool RevalidationRequired { get; }
    uint CacheInsertedAt { get; }

    /// <summary>
    /// Origin (unoptimized) content length recorded by the worker at write
    /// time. 0 when unavailable (older metadata or a front-end-written
    /// original — front-end writes never set this field). Half of the
    /// serve-stats gate: serve stats are recorded only when this
    /// is &gt; 0 AND <see cref="IsWorkerProcessed"/> is true.
    /// </summary>
    uint OriginContentLength { get; }

    /// <summary>
    /// True if this variant was written by the worker (the
    /// kFlagWorkerProcessed flag bit, 0x02, is set). The other half of the
    /// serve-stats gate. Exposed as a dedicated accessor for
    /// FFI-boundary readability — this is the same 0x02 bit the now-removed
    /// <c>RevalidationRequired</c> constant aliased (see #454).
    /// </summary>
    bool IsWorkerProcessed { get; }

    /// <summary>
    /// Re-stamps the Cyclone read lease pinning this result's mmap borrow
    ///. Any holder keeping this result alive
    /// longer than ~3.75s (3/4 of the default 5s lease) must call this at a
    /// cadence at or under 3.75s, or copy the bytes. Renewal stops
    /// protecting past the cache's 60s anti-starvation wrap ceiling — a
    /// transfer running past it is unprotected again (bounded, observable
    /// via the cache's wraps_forced_past_lease counter). Returns false when
    /// there is no lease to renew (RAM-cache hit, leases disabled, or the
    /// handle is gone).
    /// </summary>
    bool RenewLease();
}

/// <summary>Cache abstraction for testability.</summary>
public interface IPageSpeedCache : IDisposable
{
    IReadResult? ReadBest(string url, string hostname, string scheme, uint mask);

    /// <summary>
    /// Read the best agent-markdown variant via the single audited
    /// native gate. Returns null on miss or when the gate refuses a stale/unbound
    /// variant. <paramref name="agentEntitled"/> must come from
    /// <see cref="ReadSharedConfigAgentEntitled"/> AND the request's markdown intent.
    /// </summary>
    IReadResult? ReadBestAgent(string url, string hostname, string scheme, uint mask, int agentEntitled);

    /// <summary>
    /// The worker-written agent_optimize entitlement for this
    /// cache. 1 = entitled; 0 = not (incl. missing config); -1 = invalid arg.
    /// </summary>
    int ReadSharedConfigAgentEntitled();

    IReadResult? ReadAlternate(string url, string hostname, string scheme, byte alternateId);
    IReadResult? ReadEarlyHints(string url, string hostname, string scheme);
    CacheWriter BeginWrite(string url, string hostname, string scheme, CacheWriteParams p);
    CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId, ulong contentLength);
    CacheStats GetStats();

    /// <summary>
    /// Offers an origin response's complete header set (one entry per received
    /// header line) to the stored header block for this URL. The native gate
    /// decides what is carried; the caller does not curate. A response the
    /// gate refuses must not be cached. The default implementation refuses
    /// everything, so a cache that does not store headers never caches.
    /// </summary>
    HeadersSidecarWriteResult WriteHeadersSidecar(
        string url, string hostname, string scheme,
        IReadOnlyList<KeyValuePair<string, string>> headers)
        => new(HeadersSidecarVerdict.FallThrough, false);

    /// <summary>
    /// Record one worker-processed serve HIT into the shared
    /// <c>.pagespeed-serve-stats</c> mmap, mirroring the nginx front-end.
    /// The caller is responsible for the gate
    /// (<see cref="IReadResult.IsWorkerProcessed"/> AND
    /// <see cref="IReadResult.OriginContentLength"/> &gt; 0). The serve-stats
    /// handle is opened lazily (the worker is the sole creator of the file),
    /// so the first HITs after worker startup self-heal. No-op if the handle
    /// is not (yet) available. <paramref name="mask"/> is the served variant's
    /// capability mask; for image HITs whose format bits are SVG it also bumps
    /// svg.served (#455). Pass 0 to skip the SVG accounting.
    /// </summary>
    void RecordServeHit(PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask);

    // Internal handle access for HtmlProcessor.
    internal SafeCacheHandle Handle { get; }
}

/// <summary>HTML processing abstraction for testability.</summary>
public interface IHtmlProcessor
{
    /// <summary>
    /// Processes one HTML document, reading its cached stylesheets under the
    /// "https" scheme.
    /// </summary>
    HtmlResult Process(ReadOnlySpan<byte> html, string url, string hostname);

    /// <summary>
    /// Processes one HTML document, reading its cached stylesheets under
    /// <paramref name="scheme"/> ("http" or "https"), the scheme they were
    /// recorded under. An implementation that does not override this calls
    /// the three-argument form, which reads under "https".
    /// </summary>
    HtmlResult Process(
        ReadOnlySpan<byte> html, string url, string hostname, string scheme) =>
        Process(html, url, hostname);
}
