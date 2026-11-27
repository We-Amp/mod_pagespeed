// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>Exception thrown when a native PageSpeed operation fails.</summary>
public class PageSpeedException : Exception
{
    public PageSpeedError ErrorCode { get; }

    public PageSpeedException(PageSpeedError errorCode, string detail)
        : base(FormatMessage(errorCode, detail))
    {
        ErrorCode = errorCode;
    }

    private static string FormatMessage(PageSpeedError errorCode, string detail)
    {
        try
        {
            var name = NativePageSpeed.ErrorName((int)errorCode);
            var desc = NativePageSpeed.StrError((int)errorCode);
            var msg = $"{name}: {desc}";
            return string.IsNullOrEmpty(detail) ? msg : $"{msg} ({detail})";
        }
        catch
        {
            // Fallback if native library is not loaded (e.g., in tests).
            return string.IsNullOrEmpty(detail)
                ? $"PageSpeed error {(int)errorCode}"
                : $"PageSpeed error {(int)errorCode}: {detail}";
        }
    }
}
