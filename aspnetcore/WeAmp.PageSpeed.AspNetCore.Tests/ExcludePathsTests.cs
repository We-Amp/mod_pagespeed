// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Text.RegularExpressions;
using FluentAssertions;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 1: ExcludePaths has ONE definition. Each entry is a path prefix or a
/// glob. These tests pin the three consequences of that: startup validation
/// of every entry with the accepted syntax in the message, the nginx location
/// translation for each documented form (a glob like "*.json" must produce a
/// valid, anchored regex, never the verbatim pattern nginx rejects), and the
/// middleware and the generated nginx location agreeing on a shared table of
/// sample paths.
/// </summary>
public class ExcludePathsTests
{
    // Process mode (valid on every OS) with a pinned loopback origin, so the
    // generator can emit its proxy_pass target; mirrors ConfigGeneratorTests.
    private static string Generate(PageSpeedOptions options)
    {
        options.Sidecar.Mode = SidecarMode.Process;
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetPort(5000);
        var prefix = Path.Combine(Path.GetTempPath(), "psgen_" + Guid.NewGuid().ToString("N"));
        try
        {
            return new NginxConfigGenerator(
                NullLogger<NginxConfigGenerator>.Instance, endpoint).GenerateConfig(options, prefix).Conf;
        }
        finally
        {
            try { Directory.Delete(prefix, recursive: true); } catch { /* best effort */ }
        }
    }

    // The one exclude location these single-entry configs carry. The admin
    // ACL locations are the only other `~*` lines and all start with
    // /pagespeed_; the engine-owned locations use `~` or `=`. Returns the
    // quoted regex of the exclude location.
    private static string ExcludeLocationRegex(string conf)
    {
        var line = conf.Split('\n').FirstOrDefault(
            l => l.Contains("location ~* \"") && !l.Contains("/pagespeed_"));
        line.Should().NotBeNull("an ExcludePaths location must be generated");
        var m = Regex.Match(line!, "location ~\\* \"([^\"]+)\"");
        m.Success.Should().BeTrue("the exclude location line must carry a quoted regex: {0}", line);
        return m.Groups[1].Value;
    }

    // ---- startup validation ---------------------------------------------

    [Theory]
    [InlineData("/api/")]
    [InlineData("/signalr/")]
    [InlineData("/admin/*")]
    [InlineData("*.json")]
    [InlineData("/report-v?.html")]
    [InlineData("/files/v1.2/")]
    public void ValidEntries_PassStartupValidation(string pattern)
    {
        var r = new PageSpeedOptionsValidator().Validate(null, new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process },
            ExcludePaths = [pattern],
        });
        r.Succeeded.Should().BeTrue();
    }

    [Theory]
    [InlineData("^/api/")]              // the previously documented regex form
    [InlineData("/api/$")]
    [InlineData("[a-z]+/api")]
    [InlineData("/a|b/")]
    [InlineData("/api/; deny all;")]
    [InlineData("/a b/")]
    [InlineData("/a\tb/")]
    [InlineData("/secret/\\")]
    [InlineData("/ünïcode/")]
    [InlineData("")]
    [InlineData("   ")]
    public void InvalidEntries_FailStartupValidation_WithClearMessage(string pattern)
    {
        var r = new PageSpeedOptionsValidator().Validate(null, new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process },
            ExcludePaths = [pattern],
        });
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("ExcludePaths");
    }

    [Fact]
    public void RegexShapedEntry_MessagePointsAtThePrefixForm()
    {
        // '^/api/' was the DOCUMENTED example; it silently never matched in
        // the middleware. The startup message must name the accepted syntax
        // and the corrected form, not just refuse.
        var r = new PageSpeedOptionsValidator().Validate(null, new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process },
            ExcludePaths = ["^/api/"],
        });
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("regular expression");
        r.FailureMessage.Should().Contain("prefix");
        r.FailureMessage.Should().Contain("'/api/'");
    }

    [Fact]
    public void RegexBuiltOnlyFromAllowedChars_WarnsButStaysValid()
    {
        // "/api/.*" is a valid glob ('.' is a literal dot), so it must NOT be
        // rejected; it excludes only paths starting "/api/.", so the operator
        // gets a startup warning that says so instead.
        var capturing = new ListLogger<PageSpeedOptionsValidator>();
        var r = new PageSpeedOptionsValidator(capturing).Validate(null, new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process },
            ExcludePaths = ["/api/.*"],
        });
        r.Succeeded.Should().BeTrue("a glob of allowed characters is valid");
        capturing.Messages.Should().Contain(
            m => m.Contains("/api/.*") && m.Contains("literal dot"),
            "regex-intent entries that validate should warn at startup");
    }

    // ---- nginx translation: each documented form ------------------------

    [Theory]
    [InlineData("/api/", "^/api/")]
    [InlineData("/signalr/", "^/signalr/")]
    [InlineData("/files/v1.2/", "^/files/v1\\.2/")]
    [InlineData("/admin/*", "^/admin/.*$")]
    [InlineData("*.json", "^.*\\.json$")]
    [InlineData("/report-v?.html", "^/report-v.\\.html$")]
    [InlineData("*", "^.*$")]
    public void DocumentedForms_ProduceAnchoredCaseInsensitiveLocations(string pattern, string regex)
    {
        var conf = Generate(new PageSpeedOptions { ExcludePaths = [pattern] });
        conf.Should().Contain($"location ~* \"{regex}\" {{");
        conf.Should().Contain("pagespeed off;");
    }

    [Fact]
    public void GlobEntry_NeverReachesTheConfigVerbatim()
    {
        // A verbatim `location ~ "*.json"` is an invalid PCRE (a quantifier
        // with nothing to repeat); nginx -t failed and the app did not start.
        // The translated, fully anchored form must be what ships.
        var conf = Generate(new PageSpeedOptions { ExcludePaths = ["*.json"] });
        conf.Should().NotContain("\"*.json\"");
        conf.Should().Contain("location ~* \"^.*\\.json$\" {");
    }

    // ---- middleware/nginx agreement on a shared path table --------------

    // pattern, path, expected: excluded (true) or optimized (false)
    public static TheoryData<string, string, bool> AgreementTable => new()
    {
        // prefixes
        { "/api/", "/api/users", true },
        { "/api/", "/api/", true },
        { "/api/", "/api", false },
        { "/api/", "/apiv2/users", false },
        { "/api/", "/docs/api/users", false },
        { "/API/", "/api/users", true },
        { "/files/v1.2/", "/files/v1.2/x", true },
        { "/files/v1.2/", "/files/v1.25/x", false },
        // globs ('*' any sequence incl. '/', '?' exactly one char, whole path)
        { "/admin/*", "/admin/secret", true },
        { "/admin/*", "/admin", false },
        { "/admin/*", "/docs/administration", false },
        { "/admin/*", "/blog/admin-tips", false },
        { "*.json", "/reports/q1.json", true },
        { "*.json", "/reports/q1.jsonx", false },
        { "*.JSON", "/reports/q1.json", true },
        { "/report-v?.html", "/report-v2.html", true },
        { "/report-v?.html", "/report-v12.html", false },
        { "*", "/anything/at/all", true },
    };

    [Theory]
    [MemberData(nameof(AgreementTable))]
    public void MiddlewareAndGeneratedNginxLocation_Agree(string pattern, string path, bool expected)
    {
        var opts = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process },
            ExcludePaths = [pattern],
        };

        // Middleware side: the matcher the Inverse middleware consults.
        PageSpeedInverseMiddleware.IsExcluded(InverseTestSupport.NewContext(path: path).Request, opts)
            .Should().Be(expected, "the middleware must exclude '{0}' for '{1}'", path, pattern);

        // nginx side: the generated location regex, evaluated the way nginx
        // evaluates a `~*` location (case-insensitive match).
        var nginxRegex = ExcludeLocationRegex(Generate(opts));
        new Regex(nginxRegex, RegexOptions.IgnoreCase).IsMatch(path)
            .Should().Be(expected, "the nginx location regex '{0}' must agree with the middleware", nginxRegex);
    }

    // Captures formatted log messages; mirrors InverseValidatorTests.
    private sealed class ListLogger<T> : ILogger<T>
    {
        public List<string> Messages { get; } = new();
        public IDisposable BeginScope<TState>(TState state) where TState : notnull => NullScope.Instance;
        public bool IsEnabled(LogLevel logLevel) => true;
        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
            => Messages.Add(formatter(state, exception));

        private sealed class NullScope : IDisposable
        {
            public static readonly NullScope Instance = new();
            public void Dispose() { }
        }
    }
}
