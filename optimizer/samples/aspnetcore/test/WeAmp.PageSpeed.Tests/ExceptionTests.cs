// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Tests PageSpeedException construction without the native library.
/// The constructor uses a try/catch fallback when native error name/description
/// functions are unavailable.
/// </summary>
public class ExceptionTests
{
    [Fact]
    public void Constructor_WithNotFound_SetsErrorCode()
    {
        var ex = new PageSpeedException(PageSpeedError.NotFound, "test");

        Assert.Equal(PageSpeedError.NotFound, ex.ErrorCode);
    }

    [Fact]
    public void Constructor_WithNotFound_FallbackMessageContainsErrorCode()
    {
        var ex = new PageSpeedException(PageSpeedError.NotFound, "test");

        // Fallback (no native lib): "PageSpeed error 1: test".
        // Native lib loaded: "PS_ERR_NOT_FOUND: Not found (test)".
        Assert.True(ex.Message.Contains("PageSpeed error 1") ||
                    ex.Message.Contains("PS_ERR_NOT_FOUND"), ex.Message);
        Assert.Contains("test", ex.Message);
    }

    [Fact]
    public void Constructor_WithDetail_FallbackMessageContainsDetail()
    {
        var ex = new PageSpeedException(PageSpeedError.IoError, "disk full");

        Assert.Contains("disk full", ex.Message);
    }

    [Fact]
    public void Constructor_WithEmptyDetail_FallbackMessageOmitsDetail()
    {
        var ex = new PageSpeedException(PageSpeedError.Internal, "");

        // Fallback: "PageSpeed error 99"; native: "PS_ERR_INTERNAL: Internal error".
        Assert.True(ex.Message.Contains("PageSpeed error 99") ||
                    ex.Message.Contains("PS_ERR_INTERNAL"), ex.Message);
        // No detail suffix in either form.
        Assert.DoesNotContain("(", ex.Message);
    }

    [Fact]
    public void Constructor_WithOk_SetsErrorCodeToOk()
    {
        var ex = new PageSpeedException(PageSpeedError.Ok, "unexpected");

        Assert.Equal(PageSpeedError.Ok, ex.ErrorCode);
        Assert.True(ex.Message.Contains("PageSpeed error 0") ||
                    ex.Message.Contains("PS_OK"), ex.Message);
    }

    [Fact]
    public void Constructor_WithInternal_SetsErrorCodeTo99()
    {
        var ex = new PageSpeedException(PageSpeedError.Internal, "something broke");

        Assert.Equal(PageSpeedError.Internal, ex.ErrorCode);
        Assert.True(ex.Message.Contains("PageSpeed error 99") ||
                    ex.Message.Contains("PS_ERR_INTERNAL"), ex.Message);
        Assert.Contains("something broke", ex.Message);
    }

    [Fact]
    public void IsException_DerivedFromException()
    {
        var ex = new PageSpeedException(PageSpeedError.NotFound, "test");

        Assert.IsAssignableFrom<Exception>(ex);
    }

    [Fact]
    public void AllErrorCodes_CanBeConstructed()
    {
        foreach (PageSpeedError code in Enum.GetValues<PageSpeedError>())
        {
            var ex = new PageSpeedException(code, "test");
            Assert.Equal(code, ex.ErrorCode);
        }
    }
}
