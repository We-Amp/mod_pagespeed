// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// Classifies HTTP requests into capability masks. Stateless.
/// Note: HttpRequest-dependent overloads live in the ASP.NET Core library.
/// </summary>
public static class RequestClassifier
{
    /// <summary>
    /// Classify a request into a 32-bit capability mask based on
    /// Accept, User-Agent, Save-Data, and Accept-Encoding headers.
    /// </summary>
    public static uint Classify(
        string? accept, string? userAgent,
        string? saveData, string? acceptEncoding)
        => NativePageSpeed.Classify(accept, userAgent, saveData, acceptEncoding);

    /// <summary>Classify a Content-Type header string.</summary>
    public static PageSpeedContentType ClassifyContentType(string contentType)
        => (PageSpeedContentType)NativePageSpeed.ClassifyContentType(contentType);
}
