// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Http;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Headers that describe a Markdown response for agents (the page's Markdown
/// copy), the same values the nginx module sends:
/// <list type="bullet">
/// <item><c>x-markdown-tokens</c>: an ESTIMATE of the number of tokens in the
/// Markdown body, so an agent can budget its context before it reads the
/// body. The body size in bytes divided by four, rounded down; no tokenizer
/// runs, so treat the value as an order of magnitude.</item>
/// <item><c>content-signal</c>: the operator's stated use policy for the
/// content (contentsignals.org, for example
/// <c>search=yes, ai-input=yes, ai-train=no</c>). Sent only when the operator
/// configures one: the middleware never states a policy on the operator's
/// behalf.</item>
/// </list>
/// </summary>
internal static class AgentMarkdownHeaders
{
    /// <summary>Header carrying the token estimate.</summary>
    public const string TokensHeader = "x-markdown-tokens";

    /// <summary>Header carrying the operator's content-use policy.</summary>
    public const string ContentSignalHeader = "content-signal";

    /// <summary>Longest content-signal value accepted.</summary>
    public const int MaxContentSignalBytes = 512;

    /// <summary>floor(<paramref name="bodyBytes"/> / 4); 0 for a negative size.</summary>
    public static long EstimateTokens(long bodyBytes) =>
        bodyBytes <= 0 ? 0 : bodyBytes / 4;

    /// <summary>
    /// True when <paramref name="value"/> may be sent as a content-signal
    /// field value: 1 to <see cref="MaxContentSignalBytes"/> characters of
    /// visible ASCII, spaces and tabs, not starting or ending with a space or
    /// tab. A value with a line break, a control character or a non-ASCII
    /// character is refused.
    /// </summary>
    public static bool IsValidContentSignalValue(string? value)
    {
        if (string.IsNullOrEmpty(value) || value.Length > MaxContentSignalBytes)
            return false;
        foreach (char c in value)
        {
            bool visible = c >= '\x21' && c <= '\x7E';
            bool blank = c == ' ' || c == '\t';
            if (!visible && !blank) return false;
        }
        static bool IsBlank(char c) => c == ' ' || c == '\t';
        return !IsBlank(value[0]) && !IsBlank(value[^1]);
    }

    /// <summary>
    /// Adds the token estimate for a body of <paramref name="bodyBytes"/>
    /// bytes and, when <paramref name="contentSignal"/> is a valid value, the
    /// content-signal header. An invalid value is never sent (the options
    /// validator refuses it at startup and on reload; this is the second
    /// check).
    /// </summary>
    public static void Add(IHeaderDictionary headers, long bodyBytes, string? contentSignal)
    {
        headers[TokensHeader] =
            EstimateTokens(bodyBytes).ToString(System.Globalization.CultureInfo.InvariantCulture);
        if (IsValidContentSignalValue(contentSignal))
            headers[ContentSignalHeader] = contentSignal;
    }
}
