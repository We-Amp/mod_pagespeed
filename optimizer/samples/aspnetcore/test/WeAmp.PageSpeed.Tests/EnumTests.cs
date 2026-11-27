// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Validates that enum values match the C API definitions.
/// A mismatch means the managed code sends wrong values to native code.
/// </summary>
public class EnumTests
{
    // --- PageSpeedError ---

    [Fact]
    public void PageSpeedError_Ok_Is0()
    {
        Assert.Equal(0, (int)PageSpeedError.Ok);
    }

    [Fact]
    public void PageSpeedError_NotFound_Is1()
    {
        Assert.Equal(1, (int)PageSpeedError.NotFound);
    }

    [Fact]
    public void PageSpeedError_IoError_Is2()
    {
        Assert.Equal(2, (int)PageSpeedError.IoError);
    }

    [Fact]
    public void PageSpeedError_Corrupted_Is3()
    {
        Assert.Equal(3, (int)PageSpeedError.Corrupted);
    }

    [Fact]
    public void PageSpeedError_NoSpace_Is4()
    {
        Assert.Equal(4, (int)PageSpeedError.NoSpace);
    }

    [Fact]
    public void PageSpeedError_InvalidArgument_Is5()
    {
        Assert.Equal(5, (int)PageSpeedError.InvalidArgument);
    }

    [Fact]
    public void PageSpeedError_Busy_Is6()
    {
        Assert.Equal(6, (int)PageSpeedError.Busy);
    }

    [Fact]
    public void PageSpeedError_Closed_Is7()
    {
        Assert.Equal(7, (int)PageSpeedError.Closed);
    }

    [Fact]
    public void PageSpeedError_TooManyAlternates_Is8()
    {
        Assert.Equal(8, (int)PageSpeedError.TooManyAlternates);
    }

    [Fact]
    public void PageSpeedError_Exists_Is9()
    {
        Assert.Equal(9, (int)PageSpeedError.Exists);
    }

    [Fact]
    public void PageSpeedError_NotOwned_Is10()
    {
        Assert.Equal(10, (int)PageSpeedError.NotOwned);
    }

    [Fact]
    public void PageSpeedError_VersionMismatch_Is11()
    {
        Assert.Equal(11, (int)PageSpeedError.VersionMismatch);
    }

    [Fact]
    public void PageSpeedError_Internal_Is99()
    {
        Assert.Equal(99, (int)PageSpeedError.Internal);
    }

    // --- PageSpeedContentType ---

    [Fact]
    public void PageSpeedContentType_Html_Is0()
    {
        Assert.Equal(0, (int)PageSpeedContentType.Html);
    }

    [Fact]
    public void PageSpeedContentType_Css_Is1()
    {
        Assert.Equal(1, (int)PageSpeedContentType.Css);
    }

    [Fact]
    public void PageSpeedContentType_Js_Is2()
    {
        Assert.Equal(2, (int)PageSpeedContentType.Js);
    }

    [Fact]
    public void PageSpeedContentType_Image_Is3()
    {
        Assert.Equal(3, (int)PageSpeedContentType.Image);
    }

    [Fact]
    public void PageSpeedContentType_Other_Is4()
    {
        Assert.Equal(4, (int)PageSpeedContentType.Other);
    }

    // --- PageSpeedViewport ---

    [Fact]
    public void PageSpeedViewport_Mobile_Is0()
    {
        Assert.Equal(0, (int)PageSpeedViewport.Mobile);
    }

    [Fact]
    public void PageSpeedViewport_Tablet_Is1()
    {
        Assert.Equal(1, (int)PageSpeedViewport.Tablet);
    }

    [Fact]
    public void PageSpeedViewport_Desktop_Is2()
    {
        Assert.Equal(2, (int)PageSpeedViewport.Desktop);
    }
}
