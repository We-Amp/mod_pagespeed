// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers;
using System.Runtime.InteropServices;

namespace WeAmp.PageSpeed.Native;

/// <summary>
/// MemoryManager that wraps a native (mmap'd or native-allocated) pointer
/// as ReadOnlyMemory&lt;byte&gt; for zero-copy async writes.
/// Holds a DangerousAddRef on the owning SafeHandle to prevent the native
/// memory from being freed while an async write is in flight.
/// </summary>
internal sealed unsafe class NativeMemoryManager : MemoryManager<byte>
{
    private readonly byte* _pointer;
    private readonly int _length;
    private SafeHandle? _owner;
    private bool _addedRef;

    public NativeMemoryManager(byte* pointer, int length, SafeHandle owner)
    {
        _pointer = pointer;
        _length = length;
        _owner = owner;
        owner.DangerousAddRef(ref _addedRef);
    }

    public override Span<byte> GetSpan() => new(_pointer, _length);

    public override MemoryHandle Pin(int elementIndex = 0) =>
        new MemoryHandle(_pointer + elementIndex);

    public override void Unpin() { }

    protected override void Dispose(bool disposing)
    {
        var owner = Interlocked.Exchange(ref _owner, null);
        if (_addedRef && owner != null)
        {
            owner.DangerousRelease();
            _addedRef = false;
        }
    }
}
