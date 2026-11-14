// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Http;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The rule for what a request carrying a cookie may be served from, or
/// record into, the shared cache. The same answers as the native HTML cache
/// policy's cookie rule (CookieRequestMayStore / CookieRequestMayServe and
/// their inputs), on the same parsed Cache-Control values.
/// </summary>
public class CookieRequestCacheRuleTests
{
    private const ushort Hdr = CacheControlCcFlags.HeaderPresent;
    private const ushort Pub = CacheControlCcFlags.Public;
    private const ushort SMa = CacheControlCcFlags.SMaxagePresent;
    private const ushort Rep = CacheControlCcFlags.SMaxageRepeated;
    private const ushort MustReval = CacheControlCcFlags.MustRevalidate;

    [Theory]
    [InlineData(null, null, false)]
    [InlineData("Cookie", "session=abc", true)]
    [InlineData("cookie", "session=abc", true)]
    [InlineData("Cookie", "", true)]
    [InlineData("Cookie2", "$Version=1", true)]
    [InlineData("X-Cookie", "session=abc", false)]
    public void RequestHasCookie_CountsTheHeaderWhateverItsValue(
        string? name, string? value, bool expected)
    {
        var ctx = new DefaultHttpContext();
        if (name != null) ctx.Request.Headers[name] = value;
        Assert.Equal(expected, CookieRequestCacheRule.RequestHasCookie(ctx.Request));
    }

    [Theory]
    [InlineData("text/css", true)]
    [InlineData("TEXT/CSS; charset=utf-8", true)]
    [InlineData(" application/javascript ", true)]
    [InlineData("text/javascript", true)]
    [InlineData("application/x-javascript", true)]
    [InlineData("application/ecmascript", true)]
    [InlineData("text/ecmascript", true)]
    [InlineData("image/png", true)]
    [InlineData("Image/WebP", true)]
    [InlineData("image/", false)]
    [InlineData("text/html", false)]
    [InlineData("application/xhtml+xml", false)]
    [InlineData("application/json", false)]
    [InlineData("text/plain", false)]
    [InlineData("text/markdown; charset=utf-8", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void IsStaticAsset_OnlyStylesheetsScriptsAndImages(string? contentType, bool expected) =>
        Assert.Equal(expected, CookieRequestCacheRule.IsStaticAsset(contentType));

    [Theory]
    [InlineData("text/css", PageSpeedContentType.Html, true)]
    [InlineData("text/html", PageSpeedContentType.Css, false)]
    [InlineData(null, PageSpeedContentType.Css, true)]
    [InlineData(null, PageSpeedContentType.Js, true)]
    [InlineData(null, PageSpeedContentType.Image, true)]
    [InlineData(null, PageSpeedContentType.Html, false)]
    [InlineData("", PageSpeedContentType.Other, false)]
    public void StoredEntryIsStaticAsset_PrefersTheStoredContentType(
        string? originContentType, PageSpeedContentType storedType, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.StoredEntryIsStaticAsset(originContentType, storedType));

    [Theory]
    [InlineData(0, 0u, false)]
    [InlineData(Hdr, 0u, false)]
    [InlineData(Hdr | MustReval, 0u, false)]
    [InlineData(Hdr | Pub, 0u, true)]
    [InlineData(Hdr | SMa, 60u, true)]
    [InlineData(Hdr | SMa, 0u, false)]
    [InlineData(Hdr | SMa | Rep, 60u, false)]
    [InlineData(Hdr | SMa | Rep | Pub, 60u, true)]
    [InlineData(Hdr, 60u, false)]
    public void HtmlExplicitlyShared_NeedsPublicOrOnePositiveSMaxage(
        int flags, uint sMaxage, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.HtmlExplicitlyShared((ushort)flags, sMaxage));

    [Theory]
    [InlineData(0, 3600u, 0u, false)]
    [InlineData(Hdr, 0u, 0u, false)]
    [InlineData(Hdr, 3600u, 0u, true)]
    [InlineData(Hdr | SMa, 0u, 60u, true)]
    [InlineData(Hdr | SMa, 0u, 0u, false)]
    [InlineData(Hdr, 0u, 60u, false)]
    [InlineData(Hdr | Pub, 0u, 0u, false)]
    public void OriginStatesLifetime_NeedsAPositiveStatedLifetime(
        int flags, uint maxAge, uint sMaxage, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.OriginStatesLifetime((ushort)flags, maxAge, sMaxage));

    [Theory]
    [InlineData(false, false, false, false, true)]
    [InlineData(true, false, false, true, false)]
    [InlineData(true, false, true, false, true)]
    [InlineData(true, true, true, false, false)]
    [InlineData(true, true, false, true, true)]
    public void CookieRequestMayStore_PagesNeedTheMarkAssetsALifetime(
        bool hasCookie, bool staticAsset, bool explicitShared, bool lifetime, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.CookieRequestMayStore(hasCookie, staticAsset, explicitShared, lifetime));

    [Theory]
    [InlineData(false, false, false, true)]
    [InlineData(true, false, false, false)]
    [InlineData(true, false, true, true)]
    [InlineData(true, true, false, true)]
    public void CookieRequestMayServe_PagesNeedTheMarkAssetsAreServed(
        bool hasCookie, bool staticAsset, bool explicitShared, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.CookieRequestMayServe(hasCookie, staticAsset, explicitShared));

    private static readonly DateTimeOffset Now =
        new(2026, 11, 14, 12, 0, 0, TimeSpan.Zero);

    [Theory]
    [InlineData(0, "Sat, 14 Nov 2026 13:00:00 GMT", null, true, 3600u)]
    [InlineData(0, "Sat, 14 Nov 2026 11:00:00 GMT", null, true, 0u)]
    [InlineData(0, "Sat, 14 Nov 2026 13:00:00 GMT", "Sat, 14 Nov 2026 12:30:00 GMT", true, 1800u)]
    [InlineData(0, "Saturday, 14-Nov-26 13:00:00 GMT", null, true, 3600u)]
    [InlineData(0, "Sat Nov 14 13:00:00 2026", null, true, 3600u)]
    [InlineData(0, "0", null, false, 0u)]
    [InlineData(0, null, null, false, 0u)]
    [InlineData(Hdr, "Sat, 14 Nov 2026 13:00:00 GMT", null, true, 0u)]
    public void FoldExpires_CountsOnlyWithoutACacheControlLine(
        int flags, string? expires, string? date, bool headerPresent, uint maxAge)
    {
        var f = (ushort)flags;
        uint m = 0;
        CookieRequestCacheRule.FoldExpires(ref f, ref m, expires, date, Now);
        Assert.Equal(headerPresent, (f & Hdr) != 0);
        Assert.Equal(maxAge, m);
    }
}
