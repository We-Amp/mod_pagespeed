// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Http;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Whether a response's <c>Vary</c> keeps it out of the shared cache.
/// </summary>
/// <remarks>
/// <para>The cache keeps one stored response per URL and serves it to every
/// client, so a response that varies on a request header is stored only when
/// that header is <c>Accept-Encoding</c> (the stored copy is the uncompressed
/// form and the coding is chosen per request). Any other field, or <c>*</c>,
/// keeps it out. This is the native store predicate
/// (lib/cache/vary_storability.h, exported as <c>ps_vary_uncacheable</c>),
/// restated here so the check needs no native call; a test pins the two to the
/// same answers.</para>
///
/// <para>Every <c>Vary</c> token on the response counts, including those that
/// middleware ahead of this one set before it ran. Only the tokens this
/// middleware added for its own variants are left out.</para>
/// </remarks>
internal static class VaryStoreRule
{
    /// <summary>
    /// True when a response whose <c>Vary</c> lines are
    /// <paramref name="varyLines"/> may not be stored.
    /// </summary>
    public static bool Refuses(IEnumerable<string?> varyLines) =>
        Refuses(varyLines, Array.Empty<string>());

    /// <summary>
    /// As <see cref="Refuses(IEnumerable{string?})"/>, ignoring the tokens in
    /// <paramref name="ownTokens"/> (the ones this middleware added).
    /// </summary>
    public static bool Refuses(IEnumerable<string?> varyLines, IReadOnlyCollection<string> ownTokens)
    {
        foreach (var line in varyLines)
        {
            if (string.IsNullOrEmpty(line)) continue;
            foreach (var raw in line.Split(','))
            {
                var token = raw.Trim(' ', '\t');
                if (token.Length == 0) continue;
                if (token == "*") return true;
                if (ownTokens.Contains(token, StringComparer.OrdinalIgnoreCase)) continue;
                if (!token.Equals("Accept-Encoding", StringComparison.OrdinalIgnoreCase))
                    return true;
            }
        }
        return false;
    }

    /// <summary>The same question for the response as it is now.</summary>
    public static bool Refuses(HttpResponse response, IReadOnlyCollection<string> ownTokens) =>
        Refuses(response.Headers.Vary, ownTokens);
}
