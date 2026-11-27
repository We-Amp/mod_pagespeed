// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Http;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Classifies HTTP requests into capability masks. Stateless.
/// Provides HttpRequest-based convenience methods that delegate to
/// the native classification functions.
/// </summary>
public static class RequestClassifier
{
    /// <summary>
    /// Classify a request into a 32-bit capability mask based on
    /// Accept, User-Agent, Save-Data, and Accept-Encoding headers.
    /// </summary>
    public static uint Classify(HttpRequest request)
    {
        // Use implicit StringValues -> string? conversion to avoid
        // LINQ FirstOrDefault() boxing allocations.
        string? accept = request.Headers.Accept;
        string? ua = request.Headers.UserAgent;
        string? saveData = request.Headers["Save-Data"];
        string? ae = request.Headers.AcceptEncoding;
        return NativePageSpeed.Classify(accept, ua, saveData, ae);
    }

    /// <summary>Classify a Content-Type header string.</summary>
    public static PageSpeedContentType ClassifyContentType(
        string contentType) =>
        (PageSpeedContentType)NativePageSpeed.ClassifyContentType(
            contentType);

    /// <summary>
    /// Does the Accept header rank text/markdown at least as high as
    /// text/html (the agent-optimize demand signal)? Delegates to the native
    /// ps_wants_agent_markdown: text/markdown named explicitly with a q above
    /// 0 and at least the q of text/html; never matched by a wildcard Accept;
    /// equal q selects Markdown.
    /// </summary>
    public static bool WantsAgentMarkdown(HttpRequest request)
    {
        string? accept = request.Headers.Accept;
        return NativePageSpeed.WantsAgentMarkdown(accept) != 0;
    }

    /// <summary>Is the Accept header compatible with HTML responses?</summary>
    public static bool IsHtmlAccepted(HttpRequest request)
    {
        string? accept = request.Headers.Accept;
        if (accept == null) return true;
        return accept.Contains("text/html",
                   StringComparison.OrdinalIgnoreCase)
            || accept.Contains("*/*", StringComparison.Ordinal);
    }

    /// <summary>Is the response Content-Type HTML?</summary>
    public static bool IsHtmlResponse(HttpResponse response)
    {
        var ct = response.ContentType;
        return ct != null &&
               ct.StartsWith("text/html",
                   StringComparison.OrdinalIgnoreCase);
    }
}
