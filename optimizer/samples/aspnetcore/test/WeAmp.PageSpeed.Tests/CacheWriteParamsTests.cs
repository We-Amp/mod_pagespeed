// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Tests CacheWriteParams managed object construction and property accessors.
/// Note: ToNative() calls NativePageSpeed.WriteParamsInitSized which requires the
/// native library, so we only test the managed side here.
/// </summary>
public class CacheWriteParamsTests
{
    [Fact]
    public void DefaultValues_AreZero()
    {
        var p = new CacheWriteParams();

        Assert.Equal(0, p.AlternateId);
        Assert.Equal(0UL, p.ContentLength);
        Assert.Equal(0U, p.FullMask);
        Assert.Equal(PageSpeedContentType.Html, p.ContentType);
        Assert.Equal(0, p.Flags);
        Assert.Null(p.OriginContentType);
        Assert.Equal(0, p.OriginCcFlags);
    }

    [Fact]
    public void SetAlternateId_RoundTrips()
    {
        var p = new CacheWriteParams { AlternateId = 0x42 };

        Assert.Equal(0x42, p.AlternateId);
    }

    [Fact]
    public void SetContentLength_RoundTrips()
    {
        var p = new CacheWriteParams { ContentLength = 1024 * 1024 };

        Assert.Equal(1024UL * 1024, p.ContentLength);
    }

    [Fact]
    public void SetFullMask_RoundTrips()
    {
        var p = new CacheWriteParams { FullMask = 0x08 }; // Desktop/Identity

        Assert.Equal(0x08U, p.FullMask);
    }

    [Fact]
    public void SetContentType_RoundTrips()
    {
        var p = new CacheWriteParams { ContentType = PageSpeedContentType.Image };

        Assert.Equal(PageSpeedContentType.Image, p.ContentType);
    }

    [Fact]
    public void SetFlags_RoundTrips()
    {
        var p = new CacheWriteParams { Flags = CacheFlags.NeedsRevalidation };

        Assert.Equal(CacheFlags.NeedsRevalidation, p.Flags);
    }

    [Fact]
    public void SetOriginContentType_RoundTrips()
    {
        var p = new CacheWriteParams { OriginContentType = "text/html; charset=utf-8" };

        Assert.Equal("text/html; charset=utf-8", p.OriginContentType);
    }

    [Fact]
    public void SetOriginCcFlags_RoundTrips()
    {
        var p = new CacheWriteParams { OriginCcFlags = CacheControlCcFlags.MustRevalidate };

        Assert.Equal(CacheControlCcFlags.MustRevalidate, p.OriginCcFlags);
    }

    [Fact]
    public void SetMultipleOriginCcFlags_RoundTrips()
    {
        var flags = (ushort)(CacheControlCcFlags.NoCache | CacheControlCcFlags.MustRevalidate);
        var p = new CacheWriteParams { OriginCcFlags = flags };

        Assert.Equal(flags, p.OriginCcFlags);
    }

    [Fact]
    public void LargeContentLength_RoundTrips()
    {
        var p = new CacheWriteParams { ContentLength = ulong.MaxValue };

        Assert.Equal(ulong.MaxValue, p.ContentLength);
    }

    [Fact]
    public void MaxAlternateId_RoundTrips()
    {
        var p = new CacheWriteParams { AlternateId = byte.MaxValue };

        Assert.Equal(byte.MaxValue, p.AlternateId);
    }

    [Fact]
    public void AllContentTypes_CanBeSet()
    {
        foreach (PageSpeedContentType ct in Enum.GetValues<PageSpeedContentType>())
        {
            var p = new CacheWriteParams { ContentType = ct };
            Assert.Equal(ct, p.ContentType);
        }
    }
}
