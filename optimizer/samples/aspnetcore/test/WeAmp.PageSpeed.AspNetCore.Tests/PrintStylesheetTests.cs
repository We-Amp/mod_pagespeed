// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Text;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.Logging;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The standalone middleware (no worker) inlines critical CSS into the page it
/// serves while the page keeps its stylesheets, so every rule in that block
/// applies on screen. A print stylesheet's rules must never be among them
/// outside <c>@media print</c>: a print sheet that hides the header and
/// footer (the usual print layout) used to hide them on screen too.
/// Requires the native library; skips when it is absent.
/// </summary>
public class PrintStylesheetTests : IDisposable
{
    private const string ScreenCss =
        "header{display:block;background:#eee}" +
        "footer{display:block;color:#333}" +
        "p{margin:0}";

    private const string PrintCss = "header,footer{display:none}";

    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), $"ps_print_{Guid.NewGuid():N}");

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { }
    }

    public static IEnumerable<object[]> Heads() => new[]
    {
        new object[] { "link",
            "<link rel=\"stylesheet\" href=\"/screen.css\">" +
            "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\">" },
        new object[] { "style",
            "<link rel=\"stylesheet\" href=\"/screen.css\">" +
            "<style media=\"print\">header,footer{display:none}</style>" },
        new object[] { "media-block",
            "<link rel=\"stylesheet\" href=\"/screen-with-print-block.css\">" },
        new object[] { "import",
            "<link rel=\"stylesheet\" href=\"/screen-with-print-import.css\">" },
    };

    [Theory]
    [MemberData(nameof(Heads))]
    public async Task PrintRules_DoNotApplyOnScreen(string name, string head)
    {
        var html =
            "<!doctype html><html><head>" + head + "</head><body>" +
            "<header>Top</header><main><p>Hello</p></main>" +
            "<footer>Bottom</footer></body></html>";

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
            return;  // native library not available
        }

        app.UsePageSpeed();
        void Css(string path, string body) => app.MapGet(path, (HttpContext ctx) =>
        {
            ctx.Response.Headers.CacheControl = "public, max-age=3600";
            return Results.Text(body, "text/css");
        });
        Css("/screen.css", ScreenCss);
        Css("/print.css", PrintCss);
        Css("/screen-with-print-block.css",
            ScreenCss + "@media print{header,footer{display:none}}");
        Css("/screen-with-print-import.css",
            "@import url(/print.css) print;" + ScreenCss);
        app.MapGet("/", (HttpContext ctx) =>
        {
            ctx.Response.Headers.CacheControl = "public, max-age=60";
            return Results.Text(html, "text/html");
        });

        try
        {
            await app.StartAsync();
        }
        catch (DllNotFoundException)
        {
            return;  // native library not available
        }
        try
        {
            var client = app.GetTestClient();
            client.BaseAddress = new Uri("https://localhost/");
            // The stylesheets pass through the middleware first, which
            // caches them, as a browser's first visit does.
            foreach (var path in new[] { "/screen.css", "/print.css",
                         "/screen-with-print-block.css",
                         "/screen-with-print-import.css" })
            {
                (await client.GetAsync(path)).EnsureSuccessStatusCode();
            }

            var request = new HttpRequestMessage(HttpMethod.Get, "/");
            request.Headers.Accept.ParseAdd("text/html");
            var response = await client.SendAsync(request);
            response.EnsureSuccessStatusCode();
            var page = await response.Content.ReadAsStringAsync();

            var block = Regex.Match(page,
                "<style[^>]*data-pagespeed-critical[^>]*>(.*?)</style>",
                RegexOptions.Singleline);
            Assert.True(block.Success, $"{name}: no critical CSS in {page}");
            var screen = ScreenRules(block.Groups[1].Value);
            Assert.Contains("display:block", screen);
            Assert.DoesNotContain("display:none", screen);
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // The block with whitespace removed and every `@media print...{...}`
    // block cut out: the rules a screen applies.
    private static string ScreenRules(string css)
    {
        var compact = Regex.Replace(css, "\\s+", "");
        var sb = new StringBuilder();
        for (var i = 0; i < compact.Length;)
        {
            if (string.CompareOrdinal(compact, i, "@mediaprint", 0, 11) == 0)
            {
                var j = compact.IndexOf('{', i);
                if (j < 0) break;
                var depth = 0;
                for (; j < compact.Length; j++)
                {
                    if (compact[j] == '{') depth++;
                    if (compact[j] == '}' && --depth == 0) break;
                }
                i = j + 1;
                continue;
            }
            sb.Append(compact[i++]);
        }
        return sb.ToString();
    }
}
