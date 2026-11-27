// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>Result of HTML processing. Owns the transformed output.</summary>
public sealed class HtmlResult : IDisposable
{
    private readonly SafeHtmlResultHandle _handle;
    private NativeMemoryManager? _outputManager;
    private NativeMemoryManager? _earlyHintsManager;
    private int _disposed;

    internal HtmlResult(SafeHtmlResultHandle handle) => _handle = handle;

    public bool Modified
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                return NativePageSpeed.HtmlResultModified(
                    _handle.DangerousGetHandle()) != 0;
            }
            finally
            {
                if (added) _handle.DangerousRelease();
            }
        }
    }

    public bool HasCriticalCss
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                return NativePageSpeed.HtmlResultHasCriticalCss(
                    _handle.DangerousGetHandle()) != 0;
            }
            finally
            {
                if (added) _handle.DangerousRelease();
            }
        }
    }

    public bool NeedsRevalidation
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                return NativePageSpeed.HtmlResultNeedsRevalidation(
                    _handle.DangerousGetHandle()) != 0;
            }
            finally
            {
                if (added) _handle.DangerousRelease();
            }
        }
    }

    /// <summary>Transformed HTML output as ReadOnlyMemory (async-safe).
    /// The NativeMemoryManager holds a ref on the owning handle.</summary>
    public unsafe ReadOnlyMemory<byte> OutputMemory
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            if (_outputManager != null) return _outputManager.Memory;
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                var ptr = NativePageSpeed.HtmlResultOutput(
                    _handle.DangerousGetHandle(), out var len);
                if (ptr == IntPtr.Zero) return ReadOnlyMemory<byte>.Empty;
                _outputManager = new NativeMemoryManager(
                    (byte*)ptr, checked((int)len), _handle);
                return _outputManager.Memory;
            }
            finally
            {
                if (added) _handle.DangerousRelease();
            }
        }
    }

    /// <summary>Early hints payload (newline-separated URLs).</summary>
    public unsafe ReadOnlyMemory<byte> EarlyHintsMemory
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            if (_earlyHintsManager != null) return _earlyHintsManager.Memory;
            bool added = false;
            _handle.DangerousAddRef(ref added);
            try
            {
                var ptr = NativePageSpeed.HtmlResultEarlyHints(
                    _handle.DangerousGetHandle(), out var len);
                if (ptr == IntPtr.Zero) return ReadOnlyMemory<byte>.Empty;
                _earlyHintsManager = new NativeMemoryManager(
                    (byte*)ptr, checked((int)len), _handle);
                return _earlyHintsManager.Memory;
            }
            finally
            {
                if (added) _handle.DangerousRelease();
            }
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
        {
            ((IDisposable?)_outputManager)?.Dispose();
            ((IDisposable?)_earlyHintsManager)?.Dispose();
            _handle.Dispose();
        }
    }
}
