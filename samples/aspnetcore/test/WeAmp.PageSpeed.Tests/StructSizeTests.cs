// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using WeAmp.PageSpeed.Native;
using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Validates that managed struct layouts match the native C struct sizes.
/// These tests are CRITICAL for P/Invoke correctness -- a size mismatch
/// means memory corruption at runtime.
/// </summary>
public class StructSizeTests
{
    [Fact]
    public void NativeCacheConfig_Is48Bytes()
    {
        Assert.Equal(48, Marshal.SizeOf<NativeCacheConfig>());
    }

    [Fact]
    public void NativeWriteParams_Is88Bytes()
    {
        // Full ps_write_params_t since #772 (the mirror carried only the 48-byte
        // 1.1 prefix before the write path learned origin_cc_flags); 88 since
        // PS_API 1.13 appended origin_html_hash.
        Assert.Equal(88, Marshal.SizeOf<NativeWriteParams>());
    }

    [Fact]
    public void NativeAlternateInfo_Is32Bytes()
    {
        Assert.Equal(32, Marshal.SizeOf<NativeAlternateInfo>());
    }

    [Fact]
    public void NativeHtmlConfig_Is136Bytes()
    {
        // 8 + 10*4 + 2*8 + 7*8 = 120, then enable_async_css (int@120) + pad
        // to 128, then scheme (const char*@128) = 136.
        Assert.Equal(136, Marshal.SizeOf<NativeHtmlConfig>());
        // Fields are appended at the end so every prior offset is preserved;
        // pin the appended fields' offsets too.
        Assert.Equal(120, (int)Marshal.OffsetOf<NativeHtmlConfig>(
            nameof(NativeHtmlConfig.EnableAsyncCss)));
        Assert.Equal(128, (int)Marshal.OffsetOf<NativeHtmlConfig>(
            nameof(NativeHtmlConfig.Scheme)));
    }

    [Fact]
    public void NativeCriticalCssConfig_Is88Bytes()
    {
        // 8 + 3*4 + 4(pad) + 8 + 7*8 = 88
        Assert.Equal(88, Marshal.SizeOf<NativeCriticalCssConfig>());
    }

    [Fact]
    public void NativeCacheStats_Is112Bytes()
    {
        // 8 + 13*8 = 112
        Assert.Equal(112, Marshal.SizeOf<NativeCacheStats>());
    }

    [Fact]
    public void NativeCacheConfig_FieldOffsets_AreCorrect()
    {
        Assert.Equal(0, (int)Marshal.OffsetOf<NativeCacheConfig>(nameof(NativeCacheConfig.StructSize)));
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeCacheConfig>(nameof(NativeCacheConfig.VolumePath)));
        Assert.Equal(16, (int)Marshal.OffsetOf<NativeCacheConfig>(nameof(NativeCacheConfig.VolumeSize)));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeCacheConfig>(nameof(NativeCacheConfig.EnableChecksum)));
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeCacheConfig>(nameof(NativeCacheConfig.RamCacheSize)));
        Assert.Equal(40, (int)Marshal.OffsetOf<NativeCacheConfig>(nameof(NativeCacheConfig.MaxMetadataSize)));
    }

    [Fact]
    public void NativeCacheStats_FieldOffsets_AreCorrect()
    {
        Assert.Equal(0, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.StructSize)));
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.RamCacheHits)));
        Assert.Equal(16, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.RamCacheMisses)));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.DiskCacheHits)));
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.DiskCacheMisses)));
        Assert.Equal(40, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.BytesRead)));
        Assert.Equal(48, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.BytesWritten)));
        Assert.Equal(56, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.Evictions)));
        Assert.Equal(64, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.CurrentEntries)));
        Assert.Equal(72, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.CurrentSizeBytes)));
        Assert.Equal(80, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.VolumeCapacityBytes)));
        Assert.Equal(88, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.RamCacheBytes)));
        Assert.Equal(96, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.TotalHits)));
        Assert.Equal(104, (int)Marshal.OffsetOf<NativeCacheStats>(nameof(NativeCacheStats.TotalMisses)));
    }

    [Fact]
    public void NativeWriteParams_FieldOffsets_AreCorrect()
    {
        Assert.Equal(0, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.StructSize)));
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.AlternateId)));
        Assert.Equal(16, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.ContentLength)));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.FullMask)));
        Assert.Equal(28, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.ContentType)));
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.Flags)));
        Assert.Equal(40, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.OriginCt)));
        Assert.Equal(80, (int)Marshal.OffsetOf<NativeWriteParams>(nameof(NativeWriteParams.OriginHtmlHash)));
    }

    [Fact]
    public void NativeAlternateInfo_FieldOffsets_AreCorrect()
    {
        Assert.Equal(0, (int)Marshal.OffsetOf<NativeAlternateInfo>(nameof(NativeAlternateInfo.StructSize)));
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeAlternateInfo>(nameof(NativeAlternateInfo.ContentLength)));
        Assert.Equal(16, (int)Marshal.OffsetOf<NativeAlternateInfo>(nameof(NativeAlternateInfo.HitCount)));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeAlternateInfo>(nameof(NativeAlternateInfo.AlternateId)));
    }

    [Fact]
    public void NativeCriticalCssConfig_FieldOffsets_AreCorrect()
    {
        Assert.Equal(0, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.StructSize)));
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.MaxElements)));
        Assert.Equal(12, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.MaxDepth)));
        Assert.Equal(16, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.Viewport)));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.MaxCssSize)));
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.AlwaysIncludeSelectors)));
        Assert.Equal(40, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.IncludeTagPatterns)));
        Assert.Equal(48, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.IncludeClassPatterns)));
        Assert.Equal(56, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.IncludeIdPatterns)));
        Assert.Equal(64, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.ExcludeClassPatterns)));
        Assert.Equal(72, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.ExcludeIdPatterns)));
        Assert.Equal(80, (int)Marshal.OffsetOf<NativeCriticalCssConfig>(nameof(NativeCriticalCssConfig.ExcludeTagPatterns)));
    }
}
