// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Text;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The standalone middleware records a page's stylesheets under the request's
/// scheme, so it must read them back under that scheme when it builds the
/// page's critical CSS. A plain http site used to get none, because the read
/// side always looked under "https". Requires the native library; the
/// middleware tests skip when it is absent.
/// </summary>
public class HtmlSchemeTests : IDisposable
{
    private const string Css = ".hero{color:red}.unused{color:blue}";

    private const string Page =
        "<!doctype html><html><head>" +
        "<link rel=\"stylesheet\" href=\"/site.css\">" +
        "</head><body><div class=\"hero\">Hello</div></body></html>";

    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), $"ps_scheme_{Guid.NewGuid():N}");

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { }
    }

    private async Task<WebApplication?> StartAppAsync()
    {
        WebApplication app;
        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = Path.Combine(_dir, "volume.dat");
                opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
                opts.Worker.AutoStart = false;  // standalone: no worker
            });
            app = builder.Build();
        }
        catch (DllNotFoundException)
        {
            return null;  // native library not available
        }

        app.UsePageSpeed();
        app.MapGet("/site.css", (HttpContext ctx) =>
        {
            ctx.Response.Headers.CacheControl = "public, max-age=3600";
            return Results.Text(Css, "text/css");
        });
        app.MapGet("/", (HttpContext ctx) =>
        {
            ctx.Response.Headers.CacheControl = "public, max-age=60";
            return Results.Text(Page, "text/html");
        });

        try
        {
            await app.StartAsync();
        }
        catch (DllNotFoundException)
        {
            await app.DisposeAsync();
            return null;  // native library not available
        }
        return app;
    }

    private static HttpClient Client(WebApplication app, string scheme)
    {
        var client = app.GetTestClient();
        client.BaseAddress = new Uri($"{scheme}://localhost/");
        return client;
    }

    [Theory]
    [InlineData("http")]
    [InlineData("https")]
    public async Task Page_GetsCriticalCssFromItsExternalSheet(string scheme)
    {
        var app = await StartAppAsync();
        if (app is null) return;
        try
        {
            var client = Client(app, scheme);
            // The stylesheet passes through the middleware first, which
            // records it under this request's scheme.
            (await client.GetAsync("/site.css")).EnsureSuccessStatusCode();

            var request = new HttpRequestMessage(HttpMethod.Get, "/");
            request.Headers.Accept.ParseAdd("text/html");
            var response = await client.SendAsync(request);
            response.EnsureSuccessStatusCode();
            var page = await response.Content.ReadAsStringAsync();

            var block = Regex.Match(page,
                "<style[^>]*data-pagespeed-critical[^>]*>(.*?)</style>",
                RegexOptions.Singleline);
            Assert.True(block.Success, $"{scheme}: no critical CSS in {page}");
            Assert.Contains(".hero", block.Groups[1].Value);
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [Fact]
    public async Task ThreeArgumentProcess_StillReadsUnderHttps()
    {
        var app = await StartAppAsync();
        if (app is null) return;
        try
        {
            var processor = app.Services.GetRequiredService<IHtmlProcessor>();
            var html = Encoding.UTF8.GetBytes(Page);

            // Recorded under "http" only: the old form does not see it, the
            // scheme-aware form does.
            (await Client(app, "http").GetAsync("/site.css"))
                .EnsureSuccessStatusCode();
            using (var result = processor.Process(html, "/", "localhost"))
            {
                Assert.False(result.HasCriticalCss);
                Assert.True(result.NeedsRevalidation);
            }
            using (var result = processor.Process(html, "/", "localhost", "http"))
            {
                Assert.True(result.HasCriticalCss);
                Assert.False(result.NeedsRevalidation);
            }

            // Recorded under "https" as well: the old form reads it as before.
            (await Client(app, "https").GetAsync("/site.css"))
                .EnsureSuccessStatusCode();
            using (var result = processor.Process(html, "/", "localhost"))
            {
                Assert.True(result.HasCriticalCss);
                Assert.False(result.NeedsRevalidation);
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // An IHtmlProcessor written before the scheme overload existed.
    private sealed class ThreeArgumentProcessor : IHtmlProcessor
    {
        public List<(string Url, string Hostname)> Calls { get; } = new();

        public HtmlResult Process(
            ReadOnlySpan<byte> html, string url, string hostname)
        {
            Calls.Add((url, hostname));
            return null!;
        }
    }

    [Fact]
    public void ImplementationWithoutTheSchemeOverload_GetsTheThreeArgumentCall()
    {
        var impl = new ThreeArgumentProcessor();
        IHtmlProcessor processor = impl;
        processor.Process("<html></html>"u8, "/page", "example.com", "http");
        Assert.Equal(new[] { ("/page", "example.com") }, impl.Calls);
    }
}
