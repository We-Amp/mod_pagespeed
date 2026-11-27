// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Primitives;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Which request host names the application stands behind for agent
/// optimization. The optimizer worker renders pages for agents by fetching
/// from the request's host name, so that work is kept to host names the
/// application lists in <c>AllowedHosts</c>. A list that allows any host
/// (<c>*</c>), or no list at all, names none.
/// </summary>
internal static class AgentHostVouch
{
    /// <summary>
    /// True when <paramref name="allowedHosts"/> (the <c>AllowedHosts</c>
    /// entries, matched as ASP.NET Core's host filtering matches them: exact
    /// names and <c>*.example.com</c> subdomain wildcards, ports ignored)
    /// names <paramref name="host"/>, a host name without a port.
    /// </summary>
    public static bool Vouches(IList<string>? allowedHosts, string? host)
    {
        if (allowedHosts is null || allowedHosts.Count == 0 ||
            string.IsNullOrEmpty(host))
        {
            return false;
        }

        var patterns = new List<StringSegment>(allowedHosts.Count);
        foreach (var entry in allowedHosts)
        {
            var trimmed = entry?.Trim();
            if (string.IsNullOrEmpty(trimmed)) continue;
            if (IsAnyHost(trimmed)) return false;
            var name = new HostString(trimmed).Host;
            if (name.Length > 0) patterns.Add(new StringSegment(name));
        }

        return patterns.Count > 0 &&
               HostString.MatchesAny(new StringSegment(host), patterns);
    }

    // The entries host filtering treats as "any host".
    private static bool IsAnyHost(string entry) =>
        entry == "*" || entry == "[::]" || entry == "0.0.0.0";
}
