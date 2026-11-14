// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// Result of a cache read. Wraps mmap'd content -- zero-copy until disposed.
/// Returns null from cache methods on miss (not a sentinel object).
/// </summary>
/// <remarks>
/// <para>ContentMemory returns memory backed by native mmap'd data.
/// The NativeMemoryManager holds a DangerousAddRef on the SafeHandle,
/// preventing the native memory from being freed while an async write
/// is in flight. The memory becomes invalid after this ReadResult (and
/// any outstanding NativeMemoryManager) is disposed.</para>
/// </remarks>
public sealed class ReadResult : IReadResult
{
    private readonly SafeReadResultHandle _handle;
    private NativeMemoryManager? _memoryManager;
    private int _disposed;
    private ushort? _originCcFlags;

    internal ReadResult(SafeReadResultHandle handle) => _handle = handle;

    /// <summary>
    /// Returns a read-only span over the mmap'd content.
    /// Valid only while this ReadResult is not disposed.
    /// WARNING: Do not use across await boundaries -- use ContentMemory instead.
    /// </summary>
    public unsafe ReadOnlySpan<byte> Content
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                NativeCheck.ThrowOnError(
                    NativePageSpeed.ReadContent(
                        _handle.DangerousGetHandle(),
                        out var ptr, out var len));
                return new ReadOnlySpan<byte>(
                    (void*)ptr, checked((int)len));
            }
            finally
            {
                if (added) _handle.DangerousRelease();
            }
        }
    }

    /// <summary>
    /// Returns a ReadOnlyMemory wrapping the mmap'd content.
    /// Safe for async writes -- the NativeMemoryManager holds a ref on the
    /// owning SafeHandle, preventing native memory from being freed until
    /// the MemoryManager is disposed or garbage collected.
    /// </summary>
    public unsafe ReadOnlyMemory<byte> ContentMemory
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            if (_memoryManager != null)
                return _memoryManager.Memory;

            NativeCheck.ThrowOnError(
                NativePageSpeed.ReadContent(
                    _handle.DangerousGetHandle(),
                    out var ptr, out var len));
            _memoryManager = new NativeMemoryManager(
                (byte*)ptr, checked((int)len), _handle);
            return _memoryManager.Memory;
        }
    }

    /// <summary>Copy content to a destination span. Safe alternative.</summary>
    public int CopyTo(Span<byte> destination)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            NativeCheck.ThrowOnError(
                NativePageSpeed.ReadCopy(
                    _handle.DangerousGetHandle(),
                    destination, (nuint)destination.Length,
                    out var copied));
            return (int)copied;
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    public uint Mask
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try { return NativePageSpeed.ReadMask(_handle.DangerousGetHandle()); }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    public PageSpeedContentType ContentType
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                return (PageSpeedContentType)NativePageSpeed.ReadContentType(
                    _handle.DangerousGetHandle());
            }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    public string? OriginContentType
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                return NativePageSpeed.ReadOriginContentType(
                    _handle.DangerousGetHandle());
            }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    public byte Flags
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try { return NativePageSpeed.ReadFlags(_handle.DangerousGetHandle()); }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    public uint CacheInsertedAt
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try { return NativePageSpeed.ReadCacheInsertedAt(_handle.DangerousGetHandle()); }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    public uint OriginContentLength
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try { return NativePageSpeed.ReadOriginContentLength(_handle.DangerousGetHandle()); }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    public bool IsWorkerProcessed
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try { return NativePageSpeed.ReadIsWorkerProcessed(_handle.DangerousGetHandle()) != 0; }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    /// <summary>
    /// Re-stamps the Cyclone read lease pinning the mmap borrow.
    /// See <see cref="IReadResult.RenewLease"/> for the holder contract.
    /// </summary>
    public bool RenewLease()
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            return NativePageSpeed.ReadRenewLease(
                _handle.DangerousGetHandle()) != 0;
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    public bool NeedsRevalidation => (Flags & CacheFlags.NeedsRevalidation) != 0;

    /// <summary>
    /// Origin Cache-Control directives as a PS_CC_ORIGIN_* bitfield.
    /// Use ps_parse_cache_control to derive this from response headers.
    /// </summary>
    public ushort OriginCcFlags
    {
        get
        {
            if (_originCcFlags.HasValue) return _originCcFlags.Value;
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                _originCcFlags = NativePageSpeed.ReadOriginCcFlags(
                    _handle.DangerousGetHandle());
                return _originCcFlags.Value;
            }
            finally
            {
                if (added) _handle.DangerousRelease();
            }
        }
    }

    /// <summary>
    /// The origin's s-maxage in seconds as recorded with the entry; 0 when
    /// absent.
    /// </summary>
    public uint OriginSMaxage
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try { return NativePageSpeed.ReadOriginSMaxage(_handle.DangerousGetHandle()); }
            finally { if (added) _handle.DangerousRelease(); }
        }
    }

    /// <summary>
    /// Whether the origin response requires revalidation before serving.
    /// For a private cache (ASP.NET in-process), proxy-revalidate and s-maxage
    /// are ignored per RFC 9111 §5.2.2.9-10.
    /// </summary>
    public bool RevalidationRequired =>
        (OriginCcFlags & (CacheControlCcFlags.NoCache | CacheControlCcFlags.MustRevalidate)) != 0;

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
        {
            ((IDisposable?)_memoryManager)?.Dispose();
            _handle.Dispose();
        }
    }
}
