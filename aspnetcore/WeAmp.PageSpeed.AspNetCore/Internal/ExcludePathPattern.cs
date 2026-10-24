// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Text;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The single definition of a <see cref="Options.PageSpeedOptions.ExcludePaths"/>
/// entry, shared by the Inverse middleware (which matches entries against the
/// request path) and the nginx config generator (which must route the SAME
/// paths to a <c>pagespeed off</c> location). An entry is either:
/// <list type="bullet">
/// <item>a path PREFIX, matched case-insensitively with StartsWith
/// ("/api/", "/signalr/"); or</item>
/// <item>a GLOB containing '*' (any sequence) or '?' (any single character),
/// matched case-insensitively against the WHOLE path ("/admin/*",
/// "*.json").</item>
/// </list>
/// Entries are not regular expressions. The literal charset is restricted to
/// URL path characters, so a translated nginx location regex can never carry
/// a regex or nginx-config metacharacter through unescaped, and an accidental
/// regex entry ("^/api/") is rejected with a message that names the accepted
/// syntax instead of silently never matching (or breaking <c>nginx -t</c>).
/// </summary>
internal static class ExcludePathPattern
{
    // The literal characters an entry may use: RFC 3986 unreserved
    // (alphanumerics, - . _ ~), '%' (percent-encoding), the harmless
    // sub-delims (! & ' ( ) * + , =), ':' '@' and '/'. Deliberately absent:
    // every regex syntax character (^ $ [ ] | \), the nginx-config breakout
    // set (quotes ; { } #), whitespace, '$'/';' (sub-delims that are the
    // regex anchor and the directive terminator), and any non-ASCII
    // character (percent-encode those instead).
    private const string Literals =
        "abcdefghijklmnopqrstuvwxyz" +
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ" +
        "0123456789" +
        "-._~%!&'()*+,=:@/?";

    // The PCRE metacharacters that can remain after the charset check;
    // everything else allowed above is literal in both PCRE and the quoted
    // nginx location string. '*' and '?' are handled as glob wildcards, not
    // literals, by the translation below.
    private const string EscapeLiterals = ".+()";

    /// <summary>True when the entry is a glob ('*' or '?' present); false = a prefix.</summary>
    public static bool IsGlob(string pattern) =>
        pattern.Contains('*') || pattern.Contains('?');

    /// <summary>
    /// Validates one entry. Returns null when it can be matched by BOTH the
    /// middleware and the generated nginx location; otherwise a clear,
    /// operator-facing error message (suitable for both the startup options
    /// validator and the generator's fail-closed throw).
    /// </summary>
    public static string? ValidationError(string? pattern)
    {
        if (string.IsNullOrWhiteSpace(pattern))
            return "PageSpeed:ExcludePaths contains an empty entry; remove it. Entries are path prefixes " +
                "(\"/api/\") or globs ('*' matches any sequence, '?' any single character; \"/admin/*\", \"*.json\").";

        if (pattern[0] == '^' || pattern[^1] == '$')
            return $"PageSpeed:ExcludePaths entry '{pattern}' looks like a regular expression. Entries are path " +
                "prefixes (\"/api/\") or globs ('*' matches any sequence, '?' any single character; \"/admin/*\", " +
                "\"*.json\"); write '/api/' instead of '^/api/'.";

        foreach (var c in pattern)
        {
            if (Literals.IndexOf(c) < 0)
                return $"PageSpeed:ExcludePaths entry '{pattern}' contains '{c}', which cannot appear in a path " +
                    "pattern. Entries are path prefixes or globs ('*' and '?' are the only wildcards) over URL path " +
                    "characters; percent-encode anything else.";
        }

        return null;
    }

    /// <summary>
    /// Translates an entry into the nginx location regex that matches exactly
    /// the paths the middleware's IsExcluded matches: a prefix becomes an
    /// anchored prefix regex, a glob a fully anchored translation with
    /// '*' → ".*" and '?' → "." (mirroring the middleware's whole-path glob
    /// match). Emit with a case-insensitive (<c>~*</c>) location. Throws
    /// <see cref="InvalidOperationException"/> with the validation message on
    /// an invalid entry, so unvalidated input can never reach the generated
    /// config.
    /// </summary>
    public static string ToNginxLocationRegex(string pattern)
    {
        var error = ValidationError(pattern);
        if (error is not null) throw new InvalidOperationException(error);

        var sb = new StringBuilder(pattern.Length + 8);
        sb.Append('^');
        foreach (var c in pattern)
        {
            if (c == '*') sb.Append(".*");
            else if (c == '?') sb.Append('.');
            else if (EscapeLiterals.IndexOf(c) >= 0)
            {
                sb.Append('\\');
                sb.Append(c);
            }
            else sb.Append(c);
        }
        if (IsGlob(pattern)) sb.Append('$');
        return sb.ToString();
    }
}
