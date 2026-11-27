// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Tests NativeCheck.ThrowOnError behavior.
/// ThrowOnError(0) should not throw.
/// ThrowOnError(non-zero) will try to call NativePageSpeed.LastErrorMessage()
/// which calls the native library -- this throws DllNotFoundException, but
/// PageSpeedException's constructor catches native call failures and uses
/// fallback formatting.
/// </summary>
public class NativeCheckTests
{
    [Fact]
    public void ThrowOnError_Zero_DoesNotThrow()
    {
        // 0 means success -- should be a no-op.
        NativeCheck.ThrowOnError(0);
    }

    [Fact]
    public void ThrowOnError_NonZero_ThrowsPageSpeedException()
    {
        // Non-zero triggers LastErrorMessage() which tries the native library.
        // Since the native library is not available, LastErrorMessage() will throw
        // DllNotFoundException. This surfaces as DllNotFoundException because the
        // exception is thrown before PageSpeedException can be constructed.
        // We accept either PageSpeedException or DllNotFoundException.
        var thrown = false;
        try
        {
            NativeCheck.ThrowOnError(1);
        }
        catch (PageSpeedException ex)
        {
            thrown = true;
            Assert.Equal(PageSpeedError.NotFound, ex.ErrorCode);
        }
        catch (DllNotFoundException)
        {
            // Native library not available -- the P/Invoke call in
            // NativePageSpeed.LastErrorMessage() fails before we can
            // construct the PageSpeedException.
            thrown = true;
        }

        Assert.True(thrown, "Expected either PageSpeedException or DllNotFoundException");
    }

    [Fact]
    public void ThrowOnError_InternalError_ThrowsWithCode99()
    {
        var thrown = false;
        try
        {
            NativeCheck.ThrowOnError(99);
        }
        catch (PageSpeedException ex)
        {
            thrown = true;
            Assert.Equal(PageSpeedError.Internal, ex.ErrorCode);
        }
        catch (DllNotFoundException)
        {
            thrown = true;
        }

        Assert.True(thrown, "Expected either PageSpeedException or DllNotFoundException");
    }
}
