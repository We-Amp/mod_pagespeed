// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.CompilerServices;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// Checks P/Invoke return codes and throws on error.
/// MUST be called immediately after the P/Invoke call (same synchronous
/// block, no await) to capture thread-local error messages correctly.
/// </summary>
internal static class NativeCheck
{
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void ThrowOnError(int result)
    {
        if (result != 0)
        {
            // Capture thread-local error immediately — before any await.
            var detail = NativePageSpeed.LastErrorMessage();
            throw new PageSpeedException(
                (PageSpeedError)result, detail ?? "");
        }
    }
}
