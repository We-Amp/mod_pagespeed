// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;

namespace WeAmp.PageSpeed.Native;

[StructLayout(LayoutKind.Explicit, Size = 48)]
internal struct NativeCacheConfig
{
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public IntPtr VolumePath;   // const char*
    [FieldOffset(16)] public ulong VolumeSize;
    [FieldOffset(24)] public int EnableChecksum;
    [FieldOffset(32)] public nuint RamCacheSize;
    [FieldOffset(40)] public nuint MaxMetadataSize;
}

// Mirrors ps_write_params_t as of PS_API 1.13. The struct carries origin-state
// fields (ETag, insertion time, max-age / s-maxage, Last-Modified, Cache-Control
// flags) and, from 1.13, the origin_html_hash content binding (left zero:
// unbound). The 1.1 prefix is still valid via the StructSize contract.
//
// This now uses ps_write_params_init_sized (passing sizeof(NativeWriteParams))
// instead of ps_write_params_init, which clears only the 48-byte 1.1 prefix.
[StructLayout(LayoutKind.Explicit, Size = 88)]
internal struct NativeWriteParams
{
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public byte AlternateId;
    [FieldOffset(16)] public ulong ContentLength;
    [FieldOffset(24)] public uint FullMask;
    [FieldOffset(28)] public int ContentType;
    [FieldOffset(32)] public byte Flags;
    [FieldOffset(40)] public IntPtr OriginCt;      // const char*
    [FieldOffset(48)] public IntPtr OriginEtag;   // const char*
    [FieldOffset(56)] public uint CacheInsertedAt;
    [FieldOffset(60)] public uint OriginMaxAge;
    [FieldOffset(64)] public uint OriginSMaxage;
    [FieldOffset(68)] public uint OriginLastModified;
    [FieldOffset(72)] public ushort OriginCcFlags; // PS_CC_ORIGIN_* bitfield
    // Bytes 74-79 are reserved padding — no need to declare.
    [FieldOffset(80)] public IntPtr OriginHtmlHash; // const uint8_t[32] or 0
}

// Mirrors ps_cache_control_t. Parse ONE Cache-Control response header line
// into this struct, accumulating across lines. Zero and set StructSize to
// sizeof(NativeCacheControl) before the first call to ps_parse_cache_control.
[StructLayout(LayoutKind.Explicit, Size = 24)]
internal struct NativeCacheControl
{
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public uint MaxAge;
    [FieldOffset(12)] public uint SMaxage;
    [FieldOffset(16)] public ushort CcFlags; // PS_CC_ORIGIN_* bitfield
    // Bytes 18-23 are reserved padding — no need to declare.
}

[StructLayout(LayoutKind.Explicit, Size = 32)]
internal struct NativeAlternateInfo
{
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public ulong ContentLength;
    [FieldOffset(16)] public ulong HitCount;
    [FieldOffset(24)] public byte AlternateId;
    // Bytes 25-31 are padding — no need to declare.
}

// Mirrors ps_cache_stats_t. The struct carries struct_size, so future appends
// are possible. When that happens, grow Size and every offset below to match
// the native header EXACTLY. The ABI gate (tools/ci/check_abi.py) pins this
// mirror and will fail if the layout drifts.
[StructLayout(LayoutKind.Explicit, Size = 112)]
internal struct NativeCacheStats
{
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public ulong RamCacheHits;
    [FieldOffset(16)] public ulong RamCacheMisses;
    [FieldOffset(24)] public ulong DiskCacheHits;
    [FieldOffset(32)] public ulong DiskCacheMisses;
    [FieldOffset(40)] public ulong BytesRead;
    [FieldOffset(48)] public ulong BytesWritten;
    [FieldOffset(56)] public ulong Evictions;
    [FieldOffset(64)] public ulong CurrentEntries;
    [FieldOffset(72)] public ulong CurrentSizeBytes;
    [FieldOffset(80)] public ulong VolumeCapacityBytes;
    [FieldOffset(88)] public ulong RamCacheBytes;
    [FieldOffset(96)] public ulong TotalHits;
    [FieldOffset(104)] public ulong TotalMisses;
}

// Explicit layout guarantees correct alignment across all platforms.
// 10 ints (40 bytes) between two size_t fields are naturally packed,
// but Explicit removes any compiler-dependent ambiguity.
// Size = 136: the seven const char** pointers end at offset 120; the int
// enable_async_css sits at 120 and is padded to 128 under 8-byte alignment;
// the const char* scheme (PS_API 1.12) is appended at 128. The library reads
// Scheme only when StructSize covers it, so StructSize must be set to 136
// after HtmlConfigInit, which stamps the 128-byte size. MUST stay
// layout-matched with ps_html_config_t in lib/pagespeed/pagespeed.h.
[StructLayout(LayoutKind.Explicit, Size = 136)]
internal struct NativeHtmlConfig
{
    [FieldOffset(0)]   public nuint StructSize;
    [FieldOffset(8)]   public int EnableCriticalCss;
    [FieldOffset(12)]  public int EnableLazyLoad;
    [FieldOffset(16)]  public int EnableImageDimensions;
    [FieldOffset(20)]  public int EnableLcpPreload;
    [FieldOffset(24)]  public int EnablePreconnect;
    [FieldOffset(28)]  public int EnableSpeculationRules;
    [FieldOffset(32)]  public int CriticalCssMaxElements;
    [FieldOffset(36)]  public int CriticalCssMaxDepth;
    [FieldOffset(40)]  public int CssImportMaxDepth;
    [FieldOffset(44)]  public int Viewport;
    [FieldOffset(48)]  public nuint MaxHtmlSize;
    [FieldOffset(56)]  public nuint MaxCssSize;
    [FieldOffset(64)]  public IntPtr AlwaysIncludeSelectors;   // const char**
    [FieldOffset(72)]  public IntPtr IncludeTagPatterns;
    [FieldOffset(80)]  public IntPtr IncludeClassPatterns;
    [FieldOffset(88)]  public IntPtr IncludeIdPatterns;
    [FieldOffset(96)]  public IntPtr ExcludeClassPatterns;
    [FieldOffset(104)] public IntPtr ExcludeIdPatterns;
    [FieldOffset(112)] public IntPtr ExcludeTagPatterns;
    [FieldOffset(120)] public int EnableAsyncCss;
    [FieldOffset(128)] public IntPtr Scheme;                   // const char*
}

// Explicit layout required: 3 ints after size_t create a 4-byte padding gap
// before max_css_size. Sequential *happens* to work on current .NET 8, but
// Explicit is guaranteed correct and platform-independent.
[StructLayout(LayoutKind.Explicit, Size = 88)]
internal struct NativeCriticalCssConfig
{
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public int MaxElements;
    [FieldOffset(12)] public int MaxDepth;
    [FieldOffset(16)] public int Viewport;
    // 4 bytes padding at offset 20 for size_t alignment
    [FieldOffset(24)] public nuint MaxCssSize;
    [FieldOffset(32)] public IntPtr AlwaysIncludeSelectors;
    [FieldOffset(40)] public IntPtr IncludeTagPatterns;
    [FieldOffset(48)] public IntPtr IncludeClassPatterns;
    [FieldOffset(56)] public IntPtr IncludeIdPatterns;
    [FieldOffset(64)] public IntPtr ExcludeClassPatterns;
    [FieldOffset(72)] public IntPtr ExcludeIdPatterns;
    [FieldOffset(80)] public IntPtr ExcludeTagPatterns;
}
