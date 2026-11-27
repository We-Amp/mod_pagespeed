// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// Writes data to a cache alternate. Call Write() then Commit().
/// If Dispose() is called without Commit(), the write is aborted.
/// </summary>
public sealed class CacheWriter : IDisposable
{
    private readonly SafeWriteHandle _handle;
    private bool _committed;
    private int _disposed;

    internal CacheWriter(SafeWriteHandle handle) => _handle = handle;

    /// <summary>Write a chunk of data.</summary>
    public void Write(ReadOnlySpan<byte> data)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        if (_committed)
            throw new InvalidOperationException("Write already committed.");
        bool added = false;
        _handle.DangerousAddRef(ref added);
        try
        {
            NativeCheck.ThrowOnError(
                NativePageSpeed.WriteData(
                    _handle.DangerousGetHandle(), data, (nuint)data.Length));
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>
    /// Commit the write. After this call, the data is visible to readers.
    /// Must be called exactly once. Dispose without Commit aborts.
    /// </summary>
    public void Commit()
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        if (_committed)
            throw new InvalidOperationException("Write already committed.");
        _committed = true;
        int err = NativePageSpeed.WriteClose(_handle.DangerousGetHandle());
        // Always mark invalid: ps_write_close frees the native handle
        // regardless of error return, so WriteAbort must not be called.
        // SetNativeFreed also drops the reference on the owning cache.
        _handle.SetNativeFreed();
        NativeCheck.ThrowOnError(err);
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
            _handle.Dispose();
    }
}
