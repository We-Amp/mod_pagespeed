// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Win32.SafeHandles;

namespace WeAmp.PageSpeed.Native;

internal sealed class SafeCacheHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeCacheHandle() : base(ownsHandle: true) { }
    internal SafeCacheHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.CacheClose(handle);
        return true;
    }
}

/// <summary>
/// Base for native handles that borrow from an open cache (a read result
/// points into the cache's mmap; a write handle writes into it). The native
/// cache (ps_cache_close) does not reference-count its children, so the
/// managed side does: such a handle owns one SafeHandle reference on the
/// cache handle and drops it only after its own native object is freed.
/// Disposing the cache while children are outstanding therefore marks the
/// cache handle closed (new calls through it fail) but defers
/// ps_cache_close until the last child is released. This is what makes it
/// safe to retire a cache instance after a volume reset while in-flight
/// responses still stream from it.
/// </summary>
internal abstract class SafeCacheChildHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    private SafeCacheHandle? _owner;

    protected SafeCacheChildHandle() : base(ownsHandle: true) { }

    /// <param name="ptr">The native child object.</param>
    /// <param name="owner">
    /// The cache handle this child borrows from. The caller must already
    /// hold one DangerousAddRef on it; that reference is transferred to this
    /// handle and released after the child is freed. Null when the child does
    /// not borrow from a cache.
    /// </param>
    protected SafeCacheChildHandle(IntPtr ptr, SafeCacheHandle? owner)
        : base(ownsHandle: true)
    {
        SetHandle(ptr);
        _owner = owner;
    }

    protected abstract void FreeNative(IntPtr ptr);

    /// <summary>
    /// For a child whose native object was already freed by another call
    /// (ps_write_close frees the write handle): mark this handle invalid so
    /// ReleaseHandle never runs, and drop the cache reference it held.
    /// </summary>
    internal void SetNativeFreed()
    {
        SetHandleAsInvalid();
        Interlocked.Exchange(ref _owner, null)?.DangerousRelease();
    }

    protected sealed override bool ReleaseHandle()
    {
        FreeNative(handle);
        Interlocked.Exchange(ref _owner, null)?.DangerousRelease();
        return true;
    }
}

internal sealed class SafeReadResultHandle : SafeCacheChildHandle
{
    public SafeReadResultHandle() { }
    internal SafeReadResultHandle(IntPtr ptr, SafeCacheHandle? owner = null)
        : base(ptr, owner) { }
    protected override void FreeNative(IntPtr ptr) => NativePageSpeed.ReadFree(ptr);
}

internal sealed class SafeWriteHandle : SafeCacheChildHandle
{
    public SafeWriteHandle() { }
    internal SafeWriteHandle(IntPtr ptr, SafeCacheHandle? owner = null)
        : base(ptr, owner) { }

    // Abort is the safety net: releases the native handle even if
    // the caller forgot to call Commit(). Normal path calls WriteClose
    // then marks the handle released (see CacheWriter.Commit) so the
    // native handle is not closed twice.
    protected override void FreeNative(IntPtr ptr) => NativePageSpeed.WriteAbort(ptr);
}

internal sealed class SafeHtmlResultHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeHtmlResultHandle() : base(ownsHandle: true) { }
    internal SafeHtmlResultHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.HtmlResultFree(handle);
        return true;
    }
}

internal sealed class SafeScanResultHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeScanResultHandle() : base(ownsHandle: true) { }
    internal SafeScanResultHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.ScanResultFree(handle);
        return true;
    }
}

internal sealed class SafeCriticalCssResultHandle
    : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeCriticalCssResultHandle() : base(ownsHandle: true) { }
    internal SafeCriticalCssResultHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.CriticalCssResultFree(handle);
        return true;
    }
}

internal sealed class SafeHtmlTransformHandle
    : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeHtmlTransformHandle() : base(ownsHandle: true) { }
    internal SafeHtmlTransformHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.HtmlTransformFree(handle);
        return true;
    }
}
