// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Validates sentinel IDs and flag constants match the C API definitions.
/// These values are used in cache key construction -- wrong values mean
/// reading/writing to the wrong cache slots.
/// </summary>
public class ConstantsTests
{
    // --- SentinelId ---

    [Fact]
    public void SentinelId_EarlyHints_Is0x1C()
    {
        Assert.Equal(0x1C, SentinelId.EarlyHints);
    }

    [Fact]
    public void SentinelId_Warmup_Is0x2C()
    {
        Assert.Equal(0x2C, SentinelId.Warmup);
    }

    [Fact]
    public void SentinelId_ContentHash_Is0x3C()
    {
        Assert.Equal(0x3C, SentinelId.ContentHash);
    }

    [Fact]
    public void SentinelId_Subresource_Is0x4C()
    {
        Assert.Equal(0x4C, SentinelId.Subresource);
    }

    [Fact]
    public void SentinelId_BrowserProfile_Is0x5C()
    {
        Assert.Equal(0x5C, SentinelId.BrowserProfile);
    }

    // --- CacheFlags ---

    [Fact]
    public void CacheFlags_NeedsRevalidation_Is0x01()
    {
        Assert.Equal(0x01, CacheFlags.NeedsRevalidation);
    }

    // --- CacheControlCcFlags ---

    [Fact]
    public void CacheControlCcFlags_NoCache_Is0x0001()
    {
        Assert.Equal(0x0001, CacheControlCcFlags.NoCache);
    }

    [Fact]
    public void CacheControlCcFlags_MustRevalidate_Is0x0002()
    {
        Assert.Equal(0x0002, CacheControlCcFlags.MustRevalidate);
    }

    [Fact]
    public void CacheControlCcFlags_NoStore_Is0x0004()
    {
        Assert.Equal(0x0004, CacheControlCcFlags.NoStore);
    }

    [Fact]
    public void CacheControlCcFlags_Private_Is0x0008()
    {
        Assert.Equal(0x0008, CacheControlCcFlags.Private);
    }

    [Fact]
    public void CacheControlCcFlags_Public_Is0x0010()
    {
        Assert.Equal(0x0010, CacheControlCcFlags.Public);
    }

    [Fact]
    public void CacheControlCcFlags_Immutable_Is0x0020()
    {
        Assert.Equal(0x0020, CacheControlCcFlags.Immutable);
    }

    [Fact]
    public void CacheControlCcFlags_SMaxagePresent_Is0x0040()
    {
        Assert.Equal(0x0040, CacheControlCcFlags.SMaxagePresent);
    }

    [Fact]
    public void CacheControlCcFlags_ProxyRevalidate_Is0x0080()
    {
        Assert.Equal(0x0080, CacheControlCcFlags.ProxyRevalidate);
    }

    [Fact]
    public void CacheControlCcFlags_NoTransform_Is0x0100()
    {
        Assert.Equal(0x0100, CacheControlCcFlags.NoTransform);
    }

    [Fact]
    public void CacheControlCcFlags_HeaderPresent_Is0x0200()
    {
        Assert.Equal(0x0200, CacheControlCcFlags.HeaderPresent);
    }

    [Fact]
    public void CacheControlCcFlags_PrivateQualified_Is0x0400()
    {
        Assert.Equal(0x0400, CacheControlCcFlags.PrivateQualified);
    }

    [Fact]
    public void CacheControlCcFlags_PrivateBare_Is0x0800()
    {
        Assert.Equal(0x0800, CacheControlCcFlags.PrivateBare);
    }

    [Fact]
    public void CacheControlCcFlags_NoCacheQualified_Is0x1000()
    {
        Assert.Equal(0x1000, CacheControlCcFlags.NoCacheQualified);
    }

    [Fact]
    public void CacheControlCcFlags_NoCacheBare_Is0x2000()
    {
        Assert.Equal(0x2000, CacheControlCcFlags.NoCacheBare);
    }

    [Fact]
    public void CacheControlCcFlags_SMaxageRepeated_Is0x4000()
    {
        Assert.Equal(0x4000, CacheControlCcFlags.SMaxageRepeated);
    }
}
