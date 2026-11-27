// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using System.Text;

namespace WeAmp.PageSpeed.Native;

/// <summary>
/// Pins a C# string array as a null-terminated const char** for P/Invoke.
/// Must be disposed after the native call completes.
/// </summary>
internal ref struct PinnedStringArray
{
    private readonly GCHandle[] _handles;
    private GCHandle _arrayHandle;
    public IntPtr Pointer { get; }

    public PinnedStringArray(IReadOnlyList<string>? strings)
    {
        if (strings == null || strings.Count == 0)
        {
            _handles = [];
            Pointer = IntPtr.Zero;
            return;
        }
        var ptrs = new IntPtr[strings.Count + 1]; // null-terminated
        _handles = new GCHandle[strings.Count];
        for (int i = 0; i < strings.Count; i++)
        {
            var bytes = Encoding.UTF8.GetBytes(strings[i] + '\0');
            _handles[i] = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            ptrs[i] = _handles[i].AddrOfPinnedObject();
        }
        ptrs[^1] = IntPtr.Zero;
        _arrayHandle = GCHandle.Alloc(ptrs, GCHandleType.Pinned);
        Pointer = _arrayHandle.AddrOfPinnedObject();
    }

    public void Dispose()
    {
        if (_arrayHandle.IsAllocated) _arrayHandle.Free();
        foreach (var h in _handles)
            if (h.IsAllocated) h.Free();
    }
}
