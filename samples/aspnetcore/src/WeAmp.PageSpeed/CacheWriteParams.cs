// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>Parameters for beginning a cache write.</summary>
public sealed class CacheWriteParams
{
    public byte AlternateId { get; set; }
    public ulong ContentLength { get; set; }
    public uint FullMask { get; set; }
    public PageSpeedContentType ContentType { get; set; }
    public byte Flags { get; set; }
    public string? OriginContentType { get; set; }
    public ushort OriginCcFlags { get; set; }

    /// <summary>
    /// The origin's <c>s-maxage</c> in seconds, as ps_parse_cache_control
    /// reports it; 0 when absent.
    /// </summary>
    public uint OriginSMaxage { get; set; }

    internal NativeWriteParams ToNative()
    {
        var p = new NativeWriteParams();
        NativePageSpeed.WriteParamsInitSized(ref p, (nuint)Marshal.SizeOf<NativeWriteParams>());
        p.AlternateId = AlternateId;
        p.ContentLength = ContentLength;
        p.FullMask = FullMask;
        p.ContentType = (int)ContentType;
        p.Flags = Flags;
        p.OriginCcFlags = OriginCcFlags;
        p.OriginSMaxage = OriginSMaxage;
        // OriginCt pointer must be pinned by caller before CacheWriteBegin.
        return p;
    }
}
