// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using System.Text;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// Managed wrapper around the native PageSpeed cache.
/// Thread-safe for concurrent reads. Intended as a singleton in DI.
/// </summary>
public sealed class PageSpeedCache : IPageSpeedCache
{
    private readonly SafeCacheHandle _handle;
    private int _disposed;

    // Serve-stats mmap handle. The worker is the sole CREATOR of
    // {parent}/.pagespeed-serve-stats; this process only OPENs it. The file
    // may not exist yet when the cache is constructed (worker still starting),
    // so the open is lazy: IntPtr.Zero until the first successful open, then
    // cached. Guarded by _serveStatsLock so the concurrent-singleton's first
    // HITs don't race the lazy open. The native record_hit is itself lock-free
    // (relaxed atomics), so only the open is serialized.
    private readonly string _volumePath;
    private readonly object _serveStatsLock = new();
    private IntPtr _serveStats; // IntPtr.Zero until opened
    private bool _serveStatsOpened;

    public PageSpeedCache(PageSpeedCacheOptions options)
    {
        if (string.IsNullOrEmpty(options.VolumePath))
            throw new ArgumentException("VolumePath must not be null or empty.", nameof(options));

        _volumePath = options.VolumePath;

        var parentDir = Path.GetDirectoryName(options.VolumePath);
        if (!string.IsNullOrEmpty(parentDir))
        {
            try
            {
                Directory.CreateDirectory(parentDir);
            }
            catch (UnauthorizedAccessException ex)
            {
                // The default volume path (/var/cache/pagespeed/volume.dat) needs
                // elevated permissions, so a local non-root run would otherwise fail
                // here with a bare "Access to the path '/var/cache' is denied".
                throw new InvalidOperationException(
                    $"Cannot create the PageSpeed cache directory '{parentDir}' for volume " +
                    $"'{options.VolumePath}'. The default cache path requires elevated " +
                    "permissions; point the cache at a writable location (set Cache.VolumePath, " +
                    "or PageSpeed:Cache:VolumePath in ASP.NET Core configuration). See " +
                    "https://modpagespeed.com/docs/aspnet-configuration/#cache-volume.",
                    ex);
            }
        }

        var config = new NativeCacheConfig();
        NativePageSpeed.CacheConfigInit(ref config);
        config.VolumeSize = options.VolumeSizeBytes;
        config.EnableChecksum = options.EnableChecksum ? 1 : 0;
        config.RamCacheSize = (nuint)options.RamCacheSizeBytes;
        config.MaxMetadataSize = (nuint)options.MaxMetadataSizeBytes;

        var pathBytes = Encoding.UTF8.GetBytes(options.VolumePath + '\0');
        unsafe
        {
            fixed (byte* pathPtr = pathBytes)
            {
                config.VolumePath = (IntPtr)pathPtr;
                int err = NativePageSpeed.CacheOpen(in config, out var raw);
                NativeCheck.ThrowOnError(err);
                _handle = new SafeCacheHandle(raw);
            }
        }

        // Try to open the serve-stats mmap once now. If the worker hasn't
        // created the file yet (NotFound), leave it closed; RecordServeHit
        // will retry lazily on the first worker-processed HIT.
        TryOpenServeStats();
    }

    /// <summary>
    /// Attempts to open the serve-stats mmap. Returns the (possibly newly
    /// opened) handle, or IntPtr.Zero if the worker hasn't created the file
    /// yet. Idempotent and thread-safe; once opened it is cached, and a
    /// NotFound is retried on subsequent calls (so the first HITs after the
    /// worker starts self-heal). Any other native error closes the door
    /// (marks opened) to avoid thrashing the open on a permanently bad file.
    /// </summary>
    private IntPtr TryOpenServeStats()
    {
        // Fast path: once opened, _serveStats is published (Volatile.Write
        // below) before _serveStatsOpened flips, so a true read here implies a
        // fully-visible handle.
        if (Volatile.Read(ref _serveStatsOpened)) return _serveStats;
        lock (_serveStatsLock)
        {
            if (_serveStatsOpened) return _serveStats;

            int err = NativePageSpeed.ServeStatsOpen(_volumePath, out var raw);
            if (err == (int)PageSpeedError.Ok)
            {
                _serveStats = raw;
                Volatile.Write(ref _serveStatsOpened, true);
                return _serveStats;
            }
            if (err == (int)PageSpeedError.NotFound)
            {
                // Worker not up yet — keep retrying on later HITs.
                return IntPtr.Zero;
            }
            // Any other error: stop trying (treat as permanently unavailable).
            Volatile.Write(ref _serveStatsOpened, true);
            return IntPtr.Zero;
        }
    }

    SafeCacheHandle IPageSpeedCache.Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            return _handle;
        }
    }

    /// <summary>
    /// Read the best-matching alternate for the given URL and capability mask.
    /// Returns null on cache miss.
    /// </summary>
    public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            int err = NativePageSpeed.CacheReadBest(
                _handle, url, hostname, scheme, mask, out var raw);
            return WrapRead(err, raw, ref added);
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>
    /// Read the best agent-markdown variant via the SINGLE audited
    /// native gate (entitlement + content-hash binding). Returns null on miss OR
    /// when the gate refuses a stale/unbound variant (PS_ERR_NOT_FOUND), so the
    /// caller falls through to HTML. <paramref name="agentEntitled"/> MUST be
    /// derived from <see cref="ReadSharedConfigAgentEntitled"/> AND the request's
    /// markdown intent — never hard-coded.
    /// </summary>
    public IReadResult? ReadBestAgent(
        string url, string hostname, string scheme, uint mask, int agentEntitled)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            int err = NativePageSpeed.CacheReadBestAgent(
                _handle, url, hostname, scheme, mask, agentEntitled, out var raw);
            return WrapRead(err, raw, ref added);
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>
    /// The worker-written agent_optimize entitlement for this
    /// cache's volume. 1 = entitled; 0 = not entitled (incl. a missing config —
    /// the safe default); -1 = invalid argument.
    /// </summary>
    public int ReadSharedConfigAgentEntitled()
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        return NativePageSpeed.ReadSharedConfigAgentEntitled(_volumePath);
    }

    /// <summary>Read a specific alternate by ID. Returns null on miss.</summary>
    public IReadResult? ReadAlternate(
        string url, string hostname, string scheme, byte alternateId)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            int err = NativePageSpeed.CacheReadAlternate(
                _handle, url, hostname, scheme, alternateId, out var raw);
            return WrapRead(err, raw, ref added);
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>Read early hints for a URL. Returns null on miss.</summary>
    public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            int err = NativePageSpeed.CacheReadEarlyHints(
                _handle, url, hostname, scheme, out var raw);
            return WrapRead(err, raw, ref added);
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>
    /// Wraps a native read result. On success the cache-handle reference the
    /// caller took (<paramref name="added"/>) moves into the result's handle,
    /// so the native cache outlives the result even if this instance is
    /// disposed first (see <see cref="SafeCacheChildHandle"/>).
    /// </summary>
    private ReadResult? WrapRead(int err, IntPtr raw, ref bool added)
    {
        if (err == (int)PageSpeedError.NotFound) return null;
        NativeCheck.ThrowOnError(err);
        var owner = added && raw != IntPtr.Zero ? _handle : null;
        var result = new ReadResult(new SafeReadResultHandle(raw, owner));
        if (owner != null) added = false;
        return result;
    }

    /// <summary>Same transfer as <see cref="WrapRead"/>, for write handles.</summary>
    private CacheWriter WrapWrite(int err, IntPtr raw, ref bool added)
    {
        NativeCheck.ThrowOnError(err);
        var owner = added && raw != IntPtr.Zero ? _handle : null;
        var writer = new CacheWriter(new SafeWriteHandle(raw, owner));
        if (owner != null) added = false;
        return writer;
    }

    /// <summary>Begin a cache write. Dispose the writer to abort.</summary>
    public CacheWriter BeginWrite(
        string url, string hostname, string scheme, CacheWriteParams writeParams)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        var native = writeParams.ToNative();

        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            // Pin OriginContentType as UTF-8 for the duration of ps_cache_write_begin.
            if (writeParams.OriginContentType != null)
            {
                var ctBytes = System.Text.Encoding.UTF8.GetBytes(
                    writeParams.OriginContentType + '\0');
                unsafe
                {
                    fixed (byte* ctPtr = ctBytes)
                    {
                        native.OriginCt = (IntPtr)ctPtr;
                        int err = NativePageSpeed.CacheWriteBegin(
                            _handle, url, hostname, scheme, in native, out var raw);
                        return WrapWrite(err, raw, ref added);
                    }
                }
            }
            else
            {
                int err = NativePageSpeed.CacheWriteBegin(
                    _handle, url, hostname, scheme, in native, out var raw);
                return WrapWrite(err, raw, ref added);
            }
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>
    /// Begin writing a sentinel entry (e.g., early hints).
    /// Dispose the writer to abort.
    /// </summary>
    public CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId,
        ulong contentLength)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            int err = NativePageSpeed.CacheWriteSentinel(
                _handle, url, hostname, scheme, sentinelId, contentLength,
                out var raw);
            return WrapWrite(err, raw, ref added);
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>
    /// Offers a response's complete header set to the stored header block for
    /// this URL. The native gate decides what is carried and whether the
    /// response may be cached. Header text must be Latin-1 (what the server
    /// writes); anything else is refused as a fall-through.
    /// </summary>
    public HeadersSidecarWriteResult WriteHeadersSidecar(
        string url, string hostname, string scheme,
        IReadOnlyList<KeyValuePair<string, string>> headers)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            return WithHeaderTables(headers,
                new HeadersSidecarWriteResult(HeadersSidecarVerdict.FallThrough, false),
                (names, values, count) =>
                {
                    int err = NativePageSpeed.CacheWriteHeadersSidecar(
                        _handle, url, hostname, scheme, names, values, count,
                        out int verdict, out int stored);
                    NativeCheck.ThrowOnError(err);
                    return new HeadersSidecarWriteResult(
                        (HeadersSidecarVerdict)verdict, stored != 0);
                });
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>
    /// The response-header gate's verdict on <paramref name="headers"/>, the
    /// same decision <see cref="WriteHeadersSidecar"/> makes, without a cache
    /// and without storing anything.
    /// </summary>
    internal static HeadersSidecarVerdict ClassifyHeadersSidecar(
        IReadOnlyList<KeyValuePair<string, string>> headers) =>
        WithHeaderTables(headers, HeadersSidecarVerdict.FallThrough,
            (names, values, count) =>
            {
                int verdict = NativePageSpeed.HeadersSidecarClassify(names, values, count);
                return verdict is >= 0 and <= (int)HeadersSidecarVerdict.NeverOptimized
                    ? (HeadersSidecarVerdict)verdict
                    : HeadersSidecarVerdict.FallThrough;
            });

    // Lays the header lines out as two pinned tables of NUL-terminated
    // Latin-1 strings (const char* const*) and calls `call` with them.
    // A line that is not Latin-1 text returns `notLatin1` without a call.
    private static T WithHeaderTables<T>(
        IReadOnlyList<KeyValuePair<string, string>> headers, T notLatin1,
        Func<IntPtr, IntPtr, nuint, T> call)
    {
        int count = headers.Count;

        // One NUL-terminated Latin-1 string per name and per value, laid out
        // back to back; the pointer tables index into the pinned block.
        int total = 0;
        foreach (var h in headers)
        {
            if (!IsLatin1(h.Key) || !IsLatin1(h.Value))
                return notLatin1;
            total += h.Key.Length + h.Value.Length + 2;
        }
        var blob = new byte[Math.Max(total, 1)];
        var nameOffsets = new int[count];
        var valueOffsets = new int[count];
        int pos = 0;
        for (int i = 0; i < count; i++)
        {
            nameOffsets[i] = pos;
            pos += Encoding.Latin1.GetBytes(headers[i].Key, 0, headers[i].Key.Length, blob, pos);
            blob[pos++] = 0;
            valueOffsets[i] = pos;
            pos += Encoding.Latin1.GetBytes(headers[i].Value, 0, headers[i].Value.Length, blob, pos);
            blob[pos++] = 0;
        }

        var names = new IntPtr[Math.Max(count, 1)];
        var values = new IntPtr[Math.Max(count, 1)];
        var blobHandle = GCHandle.Alloc(blob, GCHandleType.Pinned);
        GCHandle namesHandle = default, valuesHandle = default;
        try
        {
            var baseAddr = blobHandle.AddrOfPinnedObject();
            for (int i = 0; i < count; i++)
            {
                names[i] = baseAddr + nameOffsets[i];
                values[i] = baseAddr + valueOffsets[i];
            }
            namesHandle = GCHandle.Alloc(names, GCHandleType.Pinned);
            valuesHandle = GCHandle.Alloc(values, GCHandleType.Pinned);
            return call(namesHandle.AddrOfPinnedObject(),
                        valuesHandle.AddrOfPinnedObject(), (nuint)count);
        }
        finally
        {
            if (valuesHandle.IsAllocated) valuesHandle.Free();
            if (namesHandle.IsAllocated) namesHandle.Free();
            blobHandle.Free();
        }
    }

    private static bool IsLatin1(string s)
    {
        // Latin-1 text with no line break or NUL (a header line cannot carry them).
        foreach (var c in s)
            if (c > 0xFF || c == '\r' || c == '\n' || c == '\0') return false;
        return true;
    }

    /// <summary>Get aggregate cache statistics.</summary>
    public CacheStats GetStats()
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        var native = new NativeCacheStats
        {
            StructSize = (nuint)Marshal.SizeOf<NativeCacheStats>()
        };
        NativeCheck.ThrowOnError(
            NativePageSpeed.CacheStats(_handle, ref native));
        return CacheStats.FromNative(native);
    }

    /// <summary>
    /// Record one worker-processed serve HIT into the shared serve-stats mmap.
    /// The caller gates on IsWorkerProcessed AND
    /// OriginContentLength &gt; 0 (the exact nginx gate). Lazily opens the mmap
    /// if the worker hadn't created it when this cache was constructed. No-op
    /// if the file still isn't available. The native record_hit is itself
    /// lock-free (relaxed atomics); the brief lock here only serializes against
    /// Dispose() unmapping the handle, not against other recorders.
    /// </summary>
    public void RecordServeHit(
        PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask)
    {
        if (_disposed != 0) return;
        var handle = TryOpenServeStats();
        if (handle == IntPtr.Zero) return;
        // Hold _serveStatsLock across the native call so Dispose() (which closes
        // the handle under the same lock, after setting _disposed) cannot unmap
        // it mid-write. The re-check of _disposed inside the lock closes the
        // last window: if Dispose already ran, we skip the (now-closed) handle.
        // Uncontended in steady state — the critical section is a single relaxed
        // atomic add; graceful shutdown drains requests before disposal anyway.
        lock (_serveStatsLock)
        {
            if (_disposed != 0) return;
            NativePageSpeed.ServeStatsRecordHit(
                handle, (int)type, originalBytes, optimizedBytes, mask);
        }
    }

    /// <summary>
    /// Closes this cache instance. Read results and writers obtained from it
    /// stay valid: each holds a reference on the native cache, which is
    /// closed only when the last of them is disposed.
    /// </summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
        {
            _handle.Dispose();
            // Close the serve-stats mmap if we ever opened it. Take the lock
            // so we don't race a lazy open in flight on another thread.
            lock (_serveStatsLock)
            {
                if (_serveStats != IntPtr.Zero)
                {
                    NativePageSpeed.ServeStatsClose(_serveStats);
                    _serveStats = IntPtr.Zero;
                }
                Volatile.Write(ref _serveStatsOpened, true); // block further lazy opens
            }
        }
    }
}
