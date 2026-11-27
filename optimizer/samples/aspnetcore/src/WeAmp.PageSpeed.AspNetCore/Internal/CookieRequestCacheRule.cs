// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Globalization;
using Microsoft.AspNetCore.Http;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// When a request that carries a cookie may be answered from, or recorded
/// into, the shared cache.
/// </summary>
/// <remarks>
/// <para>The rule is the native HTML cache policy's cookie rule
/// (lib/cache/html_cache_policy.h, "THE COOKIE RULE"), exported there as
/// <c>CookieShareClassForContentType</c>, <c>HtmlExplicitlyShared</c>,
/// <c>OriginStatesLifetime</c>, <c>HtmlCookieRequestMayShare</c>,
/// <c>CookieRequestMayStore</c> and <c>CookieRequestMayServe</c>, and applied
/// by the nginx port through src/nginx/cookie_cache_gate.h. The C API does
/// not expose those functions, so they are restated here one for one and
/// must give the same answers. Their inputs are the same: the Cache-Control
/// flags, max-age and s-maxage as <c>ps_parse_cache_control</c> reports them
/// on the record path, and as the entry stored them on the serve path.</para>
///
/// <para>For a request that carried a <c>Cookie</c> (or <c>Cookie2</c>)
/// header, any value, an empty one included:</para>
/// <list type="bullet">
/// <item>a page (anything that is not a stylesheet, script or image) is
/// recorded, and a stored page served, only when the origin marked it
/// <c>public</c> or gave it one <c>s-maxage</c> above zero. <c>s-maxage=0</c>,
/// a malformed value and a repeated <c>s-maxage</c> are no mark, and neither
/// is <c>must-revalidate</c>;</item>
/// <item>a stylesheet, script or image is recorded only when the origin
/// stated a freshness lifetime; the middleware's own default lifetime does
/// not stand in for one. A stored one is served as before.</item>
/// </list>
///
/// <para>Independent of the anonymous-request rule
/// (<see cref="PageSpeedMiddleware.IsAnonymousRequest"/>): a request must
/// pass both to be recorded.</para>
/// </remarks>
internal static class CookieRequestCacheRule
{
    /// <summary>Does the request carry a <c>Cookie</c> or <c>Cookie2</c> header, whatever its value?</summary>
    public static bool RequestHasCookie(HttpRequest request) =>
        request.Headers.ContainsKey("Cookie") ||
        request.Headers.ContainsKey("Cookie2");

    private static readonly string[] StaticAssetTypes =
    {
        "text/css", "application/javascript", "text/javascript",
        "application/x-javascript", "application/ecmascript", "text/ecmascript",
    };

    /// <summary>
    /// Mirrors <c>CookieShareClassForContentType</c>: true for a stylesheet,
    /// script or image (kStaticAsset), false for everything else, a missing
    /// value included (kPage). The media type is compared
    /// case-insensitively; parameters and surrounding whitespace are ignored.
    /// </summary>
    public static bool IsStaticAsset(string? contentType)
    {
        var media = (contentType ?? "").AsSpan();
        int semi = media.IndexOf(';');
        if (semi >= 0) media = media[..semi];
        media = media.Trim(" \t");
        if (media.Length > 6 &&
            media.StartsWith("image/", StringComparison.OrdinalIgnoreCase))
            return true;
        foreach (var type in StaticAssetTypes)
        {
            if (media.Equals(type, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    /// <summary>
    /// Mirrors the nginx port's <c>StoredEntryCookieClass</c>: the stored
    /// origin Content-Type when there is one, else the class the entry was
    /// stored under.
    /// </summary>
    public static bool StoredEntryIsStaticAsset(
        string? originContentType, PageSpeedContentType storedType)
    {
        if (!string.IsNullOrEmpty(originContentType)) return IsStaticAsset(originContentType);
        return storedType is PageSpeedContentType.Css or PageSpeedContentType.Js
            or PageSpeedContentType.Image;
    }

    /// <summary>
    /// Mirrors <c>HtmlExplicitlyShared</c>: <c>public</c>, or one
    /// <c>s-maxage</c> (<see cref="CacheControlCcFlags.SMaxageRepeated"/>
    /// clear) with a value above zero.
    /// </summary>
    public static bool HtmlExplicitlyShared(ushort originCcFlags, uint originSMaxage)
    {
        if ((originCcFlags & CacheControlCcFlags.Public) != 0) return true;
        return (originCcFlags & CacheControlCcFlags.SMaxagePresent) != 0 &&
               (originCcFlags & CacheControlCcFlags.SMaxageRepeated) == 0 &&
               originSMaxage > 0;
    }

    /// <summary>
    /// Mirrors <c>OriginStatesLifetime</c>: the flags carry
    /// <see cref="CacheControlCcFlags.HeaderPresent"/> (a Cache-Control line,
    /// or an <c>Expires</c> folded in by <see cref="FoldExpires"/>), and
    /// max-age or s-maxage is above zero.
    /// </summary>
    public static bool OriginStatesLifetime(
        ushort originCcFlags, uint originMaxAge, uint originSMaxage)
    {
        if ((originCcFlags & CacheControlCcFlags.HeaderPresent) == 0) return false;
        if (originMaxAge > 0) return true;
        return (originCcFlags & CacheControlCcFlags.SMaxagePresent) != 0 &&
               originSMaxage > 0;
    }

    /// <summary>Mirrors <c>HtmlCookieRequestMayShare</c>.</summary>
    public static bool HtmlCookieRequestMayShare(bool requestHasCookie, bool explicitShared) =>
        !requestHasCookie || explicitShared;

    /// <summary>Mirrors <c>CookieRequestMayStore</c>.</summary>
    public static bool CookieRequestMayStore(
        bool requestHasCookie, bool staticAsset, bool explicitShared,
        bool originStatesLifetime)
    {
        if (!requestHasCookie) return true;
        if (staticAsset) return originStatesLifetime;
        return HtmlCookieRequestMayShare(requestHasCookie, explicitShared);
    }

    /// <summary>Mirrors <c>CookieRequestMayServe</c>.</summary>
    public static bool CookieRequestMayServe(
        bool requestHasCookie, bool staticAsset, bool explicitShared)
    {
        if (staticAsset) return true;
        return HtmlCookieRequestMayShare(requestHasCookie, explicitShared);
    }

    /// <summary>
    /// The record path's Expires fold, as the nginx port does it: only when
    /// no Cache-Control line was sent, an <c>Expires</c> that parses as an
    /// HTTP-date sets <see cref="CacheControlCcFlags.HeaderPresent"/> and
    /// becomes the max-age (Expires minus Date, or minus
    /// <paramref name="now"/> without a usable Date; never below zero).
    /// </summary>
    public static void FoldExpires(
        ref ushort originCcFlags, ref uint originMaxAge,
        string? expires, string? date, DateTimeOffset now)
    {
        if ((originCcFlags & CacheControlCcFlags.HeaderPresent) != 0) return;
        if (!TryParseHttpDate(expires, out var expiresAt)) return;
        var from = TryParseHttpDate(date, out var dateAt) ? dateAt : now;
        var seconds = (expiresAt - from).TotalSeconds;
        originCcFlags |= CacheControlCcFlags.HeaderPresent;
        originMaxAge = seconds <= 0 ? 0
            : seconds >= uint.MaxValue ? uint.MaxValue : (uint)seconds;
    }

    // RFC 9110 §5.6.7: IMF-fixdate, and the obsolete RFC 850 and asctime forms.
    private static readonly string[] HttpDateFormats =
    {
        "r",
        "dddd, dd'-'MMM'-'yy HH':'mm':'ss 'GMT'",
        "ddd MMM d HH':'mm':'ss yyyy",
        "ddd MMM dd HH':'mm':'ss yyyy",
    };

    private static bool TryParseHttpDate(string? value, out DateTimeOffset at)
    {
        at = default;
        if (string.IsNullOrWhiteSpace(value)) return false;
        return DateTimeOffset.TryParseExact(
            value.Trim(), HttpDateFormats, CultureInfo.InvariantCulture,
            DateTimeStyles.AssumeUniversal | DateTimeStyles.AllowInnerWhite,
            out at);
    }
}
