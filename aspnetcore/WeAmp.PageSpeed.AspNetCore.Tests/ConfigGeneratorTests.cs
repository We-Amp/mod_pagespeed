using FluentAssertions;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tests for <see cref="NginxConfigGenerator"/> — the nginx-sidecar config
/// generator. Asserts the directive output AND the fail-closed
/// config-injection matrix (the D8 attack surface).
/// </summary>
public class ConfigGeneratorTests
{
    // A generator whose origin endpoint is pinned to a loopback port, so
    // GenerateConfig can emit a proxy_pass target (at runtime the Kestrel
    // configurator pins it). UDS tests pin a socket path instead. In Inverse mode
    // the nginx loopback listen port is also pinned (the generator reads it).
    private static NginxConfigGenerator NewGenerator(
        int originPort = 5000, string? socketPath = null, int nginxLoopbackPort = 0)
    {
        var endpoint = new InternalSidecarEndpoint();
        if (socketPath != null) endpoint.SetSocketPath(socketPath);
        else endpoint.SetPort(originPort);
        if (nginxLoopbackPort > 0)
        {
            endpoint.SetRawOriginLoopbackPort(originPort);
            endpoint.SetNginxLoopbackPort(nginxLoopbackPort);
        }
        return new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint);
    }

    // The pre-Inverse tests exercise the front-proxy (Process) generation path.
    // The default PageSpeedOptions.Mode flipped to Inverse, so pin Process here
    // (unless the caller already chose Inverse) — these assert the Process output,
    // which is unchanged. The Inverse path is covered by the Inverse_* tests via
    // GenerateInverse.
    private static (string Conf, string Token) Generate(
        PageSpeedOptions options, int originPort = 5000, string? socketPath = null)
    {
        if (options.Sidecar.Mode == SidecarMode.Inverse)
        {
            options.Sidecar.Mode = SidecarMode.Process;
        }
        var prefix = Path.Combine(Path.GetTempPath(), "psgen_" + Guid.NewGuid().ToString("N"));
        try
        {
            return NewGenerator(originPort, socketPath).GenerateConfig(options, prefix);
        }
        finally
        {
            try { Directory.Delete(prefix, recursive: true); } catch { /* best effort */ }
        }
    }

    // Generates the Inverse-mode config with both private ports pinned on the
    // shared endpoint (raw-origin loopback-TCP + nginx loopback listen).
    private static (string Conf, string Token) GenerateInverse(
        PageSpeedOptions options, int rawOriginPort = 5000, int nginxLoopbackPort = 5100)
    {
        options.Sidecar.Mode = SidecarMode.Inverse;
        var prefix = Path.Combine(Path.GetTempPath(), "psgen_" + Guid.NewGuid().ToString("N"));
        try
        {
            return NewGenerator(rawOriginPort, nginxLoopbackPort: nginxLoopbackPort)
                .GenerateConfig(options, prefix);
        }
        finally
        {
            try { Directory.Delete(prefix, recursive: true); } catch { /* best effort */ }
        }
    }

    // ---- happy path: directive output ----------------------------------

    [Fact]
    public void Default_ProducesValidNginxConf()
    {
        var (conf, token) = Generate(new PageSpeedOptions());

        conf.Should().Contain("load_module ");
        conf.Should().Contain("ngx_pagespeed_module.so;");
        conf.Should().Contain("worker_processes 1;");
        conf.Should().Contain("http {");
        conf.Should().Contain("server {");
        conf.Should().Contain("listen 8080;");
        conf.Should().Contain("pagespeed on;");
        conf.Should().Contain("pagespeed RewriteLevel CoreFilters;");
        conf.Should().Contain("proxy_pass http://127.0.0.1:5000;");

        token.Should().NotBeNullOrEmpty();
        token.Should().HaveLength(64); // 32 bytes hex
    }

    [Fact]
    public void CustomListenPort_IsEmitted()
    {
        var options = new PageSpeedOptions { Sidecar = new SidecarOptions { ListenPort = 9080 } };
        Generate(options).Conf.Should().Contain("listen 9080;");
    }

    [Theory]
    [InlineData("corefilters", "CoreFilters")]
    [InlineData("PASSTHROUGH", "PassThrough")]
    [InlineData("OptimizeForBandwidth", "OptimizeForBandwidth")] // the level the spike's enum missed
    [InlineData("AllFilters", "AllFilters")]
    public void RewriteLevel_IsCaseInsensitive_AndEmittedCanonical(string input, string canonical)
    {
        var options = new PageSpeedOptions { RewriteLevel = input };
        Generate(options).Conf.Should().Contain($"pagespeed RewriteLevel {canonical};");
    }

    [Fact]
    public void EnabledAndDisabledFilters_AreEmitted()
    {
        var options = new PageSpeedOptions
        {
            EnabledFilters = "rewrite_css,rewrite_javascript",
            DisabledFilters = "convert_jpeg_to_webp"
        };
        var conf = Generate(options).Conf;
        conf.Should().Contain("pagespeed EnableFilters rewrite_css,rewrite_javascript;");
        conf.Should().Contain("pagespeed DisableFilters convert_jpeg_to_webp;");
    }

    [Fact]
    public void AuthorizedDomains_AreEmitted()
    {
        var options = new PageSpeedOptions
        {
            Domains = new DomainOptions { AuthorizedDomains = ["localhost", "127.0.0.1", "*.example.com", "https://cdn.example.com:8443"] }
        };
        var conf = Generate(options).Conf;
        conf.Should().Contain("pagespeed Domain localhost;");
        conf.Should().Contain("pagespeed Domain 127.0.0.1;");
        conf.Should().Contain("pagespeed Domain *.example.com;");
        conf.Should().Contain("pagespeed Domain https://cdn.example.com:8443;");
    }

    [Fact]
    public void AuthorizedDomains_AreDeduplicated()
    {
        // ASP.NET config-binding appends to the non-empty default list, so dups
        // are common; the generator must emit each Domain once (no nginx warning).
        var options = new PageSpeedOptions
        {
            Domains = new DomainOptions { AuthorizedDomains = ["localhost", "localhost", "127.0.0.1"] }
        };
        var conf = Generate(options).Conf;
        System.Text.RegularExpressions.Regex.Matches(conf, @"pagespeed Domain localhost;").Count.Should().Be(1);
    }

    [Fact]
    public void CustomOptions_SafeTuningKey_IsEmitted()
    {
        var options = new PageSpeedOptions
        {
            CustomOptions = { ["CssInlineMaxBytes"] = "2048", ["JpegRecompressionQuality"] = "85" }
        };
        var conf = Generate(options).Conf;
        conf.Should().Contain("pagespeed CssInlineMaxBytes 2048;");
        conf.Should().Contain("pagespeed JpegRecompressionQuality 85;");
    }

    [Fact]
    public void HandlerLocations_AndHealthStub_ArePresent()
    {
        var conf = Generate(new PageSpeedOptions()).Conf;
        conf.Should().Contain("\\.pagespeed\\.([a-z]\\.)?[a-z]{2}\\.[^.]{10}\\.[^.]+");
        conf.Should().Contain("location ~ \"^/pagespeed_static/\"");
        conf.Should().Contain("location ~ \"^/ngx_pagespeed_beacon$\"");
        conf.Should().Contain("location = /pagespeed/health { return 200 \"pagespeed-sidecar-ok\\n\"; }");
    }

    [Fact]
    public void AdminEndpoints_DefaultToLoopbackOnly_WithBearerMap()
    {
        var conf = Generate(new PageSpeedOptions()).Conf;

        // The bearer map (one line, rotation-friendly) + the loopback ACL.
        // map_hash_bucket_size is raised because the "Bearer <64-hex>" key (~71B)
        // exceeds nginx's default 64 (validated end-to-end: nginx [emerg] without it).
        conf.Should().Contain("map_hash_bucket_size 128;");
        conf.Should().Contain("map $http_authorization $admin_ok {");
        conf.Should().Contain("default 0;");
        conf.Should().Contain("allow 127.0.0.1/32;");
        conf.Should().Contain("allow ::1/128;");
        conf.Should().Contain("deny all;");
        conf.Should().Contain("if ($admin_ok = 0) { return 403; }");

        // Module-level loopback gate (location-INDEPENDENT — closes the precontent
        // admin-dispatcher bypass) + case-insensitive `~*` admin locations ordered
        // before the .pagespeed./ExcludePaths regexes so they cannot shadow the ACL
        // and a case-variant URI cannot fall through to `location /`.
        conf.Should().Contain("pagespeed StrictAdminAccess on;");
        // All FIVE module-dispatched admin paths must carry the case-insensitive ACL
        // location, or an uncovered one silently regresses to a bypassable form.
        conf.Should().Contain("location ~* \"^/pagespeed_admin\" {");
        conf.Should().Contain("location ~* \"^/pagespeed_statistics\" {");
        conf.Should().Contain("location ~* \"^/pagespeed_global_statistics\" {");
        conf.Should().Contain("location ~* \"^/pagespeed_message\" {");
        conf.Should().Contain("location ~* \"^/pagespeed_console\" {");
    }

    [Fact]
    public void AdminLocations_AreCaseInsensitive_AndOrderedBeforeResourceRegex()
    {
        // the module dispatches admin/stats/console case-INSENSITIVELY
        // in the precontent phase, so a case-sensitive prefix would let
        // GET /PAGESPEED_ADMIN bypass the nginx ACL. The admin locations must be
        // case-insensitive (~*) AND emitted before the .pagespeed. resource regex so
        // first-regex-wins keeps them from being shadowed.
        var conf = Generate(new PageSpeedOptions()).Conf;

        conf.Should().Contain("location ~* \"^/pagespeed_admin\" {");
        var adminIdx = conf.IndexOf("location ~* \"^/pagespeed_admin\"", StringComparison.Ordinal);
        var resourceIdx = conf.IndexOf("\\.pagespeed\\.", StringComparison.Ordinal);
        adminIdx.Should().BeGreaterThan(-1);
        resourceIdx.Should().BeGreaterThan(-1);
        adminIdx.Should().BeLessThan(resourceIdx,
            "admin ACL regex must precede the .pagespeed. resource regex (first-regex-wins)");
        conf.Should().NotContain("location ^~ /pagespeed_admin",
            "the bypassable case-sensitive prefix form must be gone");
    }

    [Fact]
    public void AdminRateLimit_IsWired_ByDefault()
    {
        // SPEC-3: RateLimitRpm (default 60) must emit a real limit_req zone + a
        // per-admin-location limit_req — no longer a silent no-op security knob.
        var conf = Generate(new PageSpeedOptions()).Conf;

        conf.Should().Contain("limit_req_zone $binary_remote_addr zone=pagespeed_admin:1m rate=60r/m;");
        conf.Should().Contain("limit_req zone=pagespeed_admin burst=10 nodelay;");
    }

    [Fact]
    public void AdminRateLimit_Zero_DisablesIt()
    {
        var options = new PageSpeedOptions { AdminAuth = new AdminAuthOptions { RateLimitRpm = 0 } };
        var conf = Generate(options).Conf;

        conf.Should().NotContain("limit_req_zone");
        conf.Should().NotContain("limit_req zone=pagespeed_admin");
    }

    [Fact]
    public void AdminRateLimit_NotEmitted_WhenAdminDisabled()
    {
        var options = new PageSpeedOptions { AdminAuth = new AdminAuthOptions { Enabled = false } };
        Generate(options).Conf.Should().NotContain("limit_req");
    }

    [Fact]
    public void AdminRateLimit_CustomRpm_IsEmitted()
    {
        var options = new PageSpeedOptions { AdminAuth = new AdminAuthOptions { RateLimitRpm = 120 } };
        Generate(options).Conf.Should().Contain("rate=120r/m;");
    }

    [Fact]
    public void AdminAuth_CustomAllowedIps_AreValidatedAndEmitted()
    {
        var options = new PageSpeedOptions
        {
            AdminAuth = new AdminAuthOptions { AllowedIps = ["127.0.0.1/32", "10.0.0.0/8", "192.168.1.5"] }
        };
        var conf = Generate(options).Conf;
        conf.Should().Contain("allow 10.0.0.0/8;");
        conf.Should().Contain("allow 192.168.1.5;");
    }

    [Fact]
    public void AdminAuth_Disabled_KeepsIpAclButDropsBearer()
    {
        var options = new PageSpeedOptions { AdminAuth = new AdminAuthOptions { Enabled = false } };
        var conf = Generate(options).Conf;
        conf.Should().Contain("deny all;");                       // IP ACL still applies (fail-safe)
        conf.Should().NotContain("map $http_authorization");      // no bearer map
        conf.Should().NotContain("$admin_ok");
    }

    [Fact]
    public void ExplicitAdminToken_IsHonored_AndAppearsInTheMap()
    {
        var options = new PageSpeedOptions
        {
            AdminAuth = new AdminAuthOptions { Token = "my-secret.token_value" }
        };
        var (conf, token) = Generate(options);
        token.Should().Be("my-secret.token_value");
        conf.Should().Contain("\"Bearer my-secret.token_value\" 1;");
    }

    [Fact]
    public void GeneratedTokens_AreUniquePerCall()
    {
        var (_, t1) = Generate(new PageSpeedOptions());
        var (_, t2) = Generate(new PageSpeedOptions());
        t1.Should().NotBe(t2);
    }

    [Fact]
    public void UnixSocketOrigin_EmitsUnixProxyPass()
    {
        var conf = Generate(new PageSpeedOptions(), socketPath: "/tmp/ps-1-abcd1234/origin.sock").Conf;
        conf.Should().Contain("proxy_pass http://unix:/tmp/ps-1-abcd1234/origin.sock:;");
    }

    [Fact]
    public void ExcludePaths_AreEmittedAsPagespeedOffLocations()
    {
        var options = new PageSpeedOptions { ExcludePaths = ["^/api/", "^/signalr/"] };
        var conf = Generate(options).Conf;
        conf.Should().Contain("location ~ \"^/api/\" {");
        conf.Should().Contain("location ~ \"^/signalr/\" {");
        conf.Should().Contain("pagespeed off;");
    }

    [Fact]
    public void Generate_WhenOriginEndpointNotPinned_ThrowsClearly()
    {
        var generator = new NginxConfigGenerator(
            NullLogger<NginxConfigGenerator>.Instance, new InternalSidecarEndpoint());
        var prefix = Path.Combine(Path.GetTempPath(), "psgen_" + Guid.NewGuid().ToString("N"));
        try
        {
            Action act = () => generator.GenerateConfig(new PageSpeedOptions(), prefix);
            act.Should().Throw<InvalidOperationException>()
                .WithMessage("*no transport set*");
        }
        finally { try { Directory.Delete(prefix, true); } catch { } }
    }

    // ---- config-injection matrix: every class must fail closed ---------
    //

    [Fact]
    public void Injection1_DomainBreakout_FailsClosed()
    {
        var options = new PageSpeedOptions
        {
            Domains = new DomainOptions { AuthorizedDomains = ["evil.com; } server { listen 81; #"] }
        };
        Invoking(options).Should().Throw<Exception>();
    }

    [Fact]
    public void Injection2_CustomOptionsDeniedKey_FailsClosed()
    {
        // Admin/path/fetch/script directives must never be reachable via CustomOptions.
        foreach (var denied in new[] { "AdminPath", "LoadFromFile", "FileCachePath", "RemoteConfigurationUrl" })
        {
            var options = new PageSpeedOptions { CustomOptions = { [denied] = "/etc/passwd" } };
            Invoking(options).Should().Throw<InvalidOperationException>(
                $"CustomOptions key '{denied}' must be rejected (default-deny)");
        }
    }

    [Fact]
    public void Injection3_CustomOptionsValueMetacharacter_FailsClosed()
    {
        var options = new PageSpeedOptions
        {
            CustomOptions = { ["CssInlineMaxBytes"] = "2048;\n  } server { listen 82;" }
        };
        Invoking(options).Should().Throw<InvalidOperationException>();
    }

    [Fact]
    public void Injection4_ExcludePathMetacharacter_FailsClosed()
    {
        var options = new PageSpeedOptions { ExcludePaths = ["^/api/\" { deny all; } location / { proxy_pass http://evil"] };
        Invoking(options).Should().Throw<InvalidOperationException>();
    }

    [Fact]
    public void Injection5_RewriteLevelNotInEnum_FailsClosed()
    {
        Invoking(new PageSpeedOptions { RewriteLevel = "AllFilters; pagespeed Disallow *" })
            .Should().Throw<InvalidOperationException>();
    }

    [Theory]
    [InlineData(0)]
    [InlineData(70000)]
    [InlineData(-1)]
    public void Injection6_ListenPortOutOfRange_FailsClosed(int port)
    {
        Invoking(new PageSpeedOptions { Sidecar = new SidecarOptions { ListenPort = port } })
            .Should().Throw<InvalidOperationException>();
    }

    [Fact]
    public void Injection7_FilterNameNonCharset_FailsClosed()
    {
        Invoking(new PageSpeedOptions { EnabledFilters = "rewrite_css; } server {" })
            .Should().Throw<InvalidOperationException>();
    }

    [Fact]
    public void Injection8_FileCachePathMetacharacter_FailsClosed()
    {
        var options = new PageSpeedOptions
        {
            Cache = new CacheOptions { FileCachePath = "/tmp/x; } server { listen 83; #" }
        };
        Invoking(options).Should().Throw<InvalidOperationException>();
    }

    [Fact]
    public void Injection9_MaliciousSocketPath_FailsClosed()
    {
        // Sidecar.SocketPath is operator-tainted and flows verbatim into proxy_pass;
        // it must be validated fail-closed at the endpoint choke point.
        var endpoint = new InternalSidecarEndpoint();
        endpoint.Invoking(e => e.SetSocketPath("/run/app.sock; } location = /open { proxy_pass http://unix:/run/app.sock"))
            .Should().Throw<ArgumentException>();
        // a clean absolute path is accepted
        new InternalSidecarEndpoint().Invoking(e => e.SetSocketPath("/tmp/ps-1-abcd/origin.sock"))
            .Should().NotThrow();
    }

    [Fact]
    public void Injection_ExcludePathTrailingBackslash_FailsClosed()
    {
        // A trailing odd backslash would escape the emitted closing quote.
        Invoking(new PageSpeedOptions { ExcludePaths = ["^/secret/\\"] })
            .Should().Throw<InvalidOperationException>();
    }

    [Fact]
    public void AdminToken_TooLong_FailsClosed()
    {
        // An over-long operator token would overflow map_hash_bucket_size.
        Invoking(new PageSpeedOptions { AdminAuth = new AdminAuthOptions { Token = new string('a', 101) } })
            .Should().Throw<InvalidOperationException>();
    }

    [Theory]
    [InlineData("20\v48")]      // VT
    [InlineData("20\f48")]      // FF
    [InlineData("20\u008548")]  // NEL
    [InlineData("20\u202848")]  // LS (line separator)
    [InlineData("20\u202948")]  // PS (paragraph separator)
    public void Injection_ExoticWhitespaceInCustomOptionsValue_FailsClosed(string value)
    {
        // defense-in-depth — exotic whitespace/control chars never reach the
        // generated config even though nginx wouldn't tokenize on them.
        Invoking(new PageSpeedOptions { CustomOptions = { ["CssInlineMaxBytes"] = value } })
            .Should().Throw<InvalidOperationException>();
    }

    [Theory]
    [InlineData("^/a\vpi/")]      // VT
    [InlineData("^/a\fpi/")]      // FF
    [InlineData("^/a\u0085pi/")]  // NEL
    [InlineData("^/a\u2028pi/")]  // LS
    [InlineData("^/a\u2029pi/")]  // PS
    public void Injection_ExoticWhitespaceInExcludePath_FailsClosed(string pattern)
    {
        // the ExcludePaths regex reject set rejects the same control chars.
        Invoking(new PageSpeedOptions { ExcludePaths = [pattern] })
            .Should().Throw<InvalidOperationException>();
    }

    private static Action Invoking(PageSpeedOptions options) => () => Generate(options);

    // ---- Inverse-mode generation ---------------------

    [Fact]
    public void Inverse_ListenIsLoopbackOnly()
    {
        var conf = GenerateInverse(new PageSpeedOptions(), nginxLoopbackPort: 5100).Conf;
        conf.Should().Contain("listen 127.0.0.1:5100;",
            "in Inverse nginx listens loopback-only on the private NginxLoopbackPort");
        conf.Should().NotContain("listen 8080;",
            "the bare public listen must be absent in Inverse (Kestrel owns the public socket)");
    }

    [Fact]
    public void Inverse_ProxyPassTargetsPrivateRawOrigin()
    {
        var conf = GenerateInverse(new PageSpeedOptions(), rawOriginPort: 5000, nginxLoopbackPort: 5100).Conf;
        // proxy_pass to the private raw-origin loopback-TCP endpoint (where the
        // middleware bypasses itself), NOT the public Kestrel port.
        conf.Should().Contain("proxy_pass http://127.0.0.1:5000;");
    }

    [Fact]
    public void Inverse_ForwardsXForwardedProto()
    {
        var conf = GenerateInverse(new PageSpeedOptions(), nginxLoopbackPort: 5100).Conf;
        conf.Should().Contain("proxy_set_header X-Forwarded-Proto $http_x_forwarded_proto;");
        conf.Should().Contain("pagespeed RespectXForwardedProto on;");
    }

    [Fact]
    public void Inverse_ForcesIdentityToRawOrigin()
    {
        var conf = GenerateInverse(new PageSpeedOptions(), nginxLoopbackPort: 5100).Conf;
        conf.Should().Contain("proxy_set_header Accept-Encoding \"\";",
            "the nginx->raw-origin hop must force identity so nginx receives parseable bytes");
    }

    [Fact]
    public void Inverse_PreservesHostHeader()
    {
        var conf = GenerateInverse(new PageSpeedOptions(), nginxLoopbackPort: 5100).Conf;
        conf.Should().Contain("proxy_set_header Host $host;");
    }

    [Fact]
    public void Inverse_HandlerLocationsAndHealthStub_StillPresent()
    {
        var conf = GenerateInverse(new PageSpeedOptions(), nginxLoopbackPort: 5100).Conf;
        conf.Should().Contain("\\.pagespeed\\.([a-z]\\.)?[a-z]{2}\\.[^.]{10}\\.[^.]+");
        conf.Should().Contain("location ~ \"^/pagespeed_static/\"");
        conf.Should().Contain("location ~ \"^/ngx_pagespeed_beacon$\"");
        conf.Should().Contain("location = /pagespeed/health { return 200 \"pagespeed-sidecar-ok\\n\"; }");
    }

    [Fact]
    public void Inverse_AdminLocationsAndBearer_StillPresent()
    {
        var conf = GenerateInverse(new PageSpeedOptions(), nginxLoopbackPort: 5100).Conf;
        conf.Should().Contain("pagespeed StrictAdminAccess on;");
        conf.Should().Contain("location ~* \"^/pagespeed_admin\" {");
        conf.Should().Contain("map $http_authorization $admin_ok {");
        conf.Should().Contain("if ($admin_ok = 0) { return 403; }");
        conf.Should().Contain("allow 127.0.0.1/32;");
        conf.Should().Contain("deny all;");
    }

    [Fact]
    public void FrontProxyAndExternalModes_Unchanged()
    {
        // Regression: the Process path keeps the public `listen <port>;` and does
        // NOT emit the Inverse-only XFP/identity headers or the loopback listen.
        var process = Generate(new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process, ListenPort = 8080 }
        }).Conf;
        process.Should().Contain("listen 8080;");
        process.Should().NotContain("listen 127.0.0.1:");
        process.Should().NotContain("proxy_set_header X-Forwarded-Proto $http_x_forwarded_proto;");
        process.Should().NotContain("proxy_set_header Accept-Encoding \"\";");
        process.Should().NotContain("pagespeed RespectXForwardedProto on;");
    }

    [Fact]
    public void Inverse_InjectionMatrix_StillFailsClosed()
    {
        // The fail-closed validation runs on the Inverse path too (it precedes the
        // server-scope emission). A Domain breakout must still throw.
        Action act = () => GenerateInverse(new PageSpeedOptions
        {
            Domains = new DomainOptions { AuthorizedDomains = ["evil.com; } server { listen 81; #"] }
        }, nginxLoopbackPort: 5100);
        act.Should().Throw<Exception>();

        // An ExcludePaths block on the Inverse path also forwards XFP + identity.
        var conf = GenerateInverse(new PageSpeedOptions { ExcludePaths = ["^/api/"] }, nginxLoopbackPort: 5100).Conf;
        conf.Should().Contain("location ~ \"^/api/\" {");
        conf.Should().Contain("proxy_set_header X-Forwarded-Proto $http_x_forwarded_proto;");
    }

    // ---- unmapped-surface warnings (WarnUnmappedOptions) ----------------

    // Minimal ILogger that records warning-level messages so we can assert the
    // "dropped surface gets a note" contract without a mocking framework.
    private sealed class CapturingLogger<T> : ILogger<T>
    {
        public readonly List<string> Warnings = [];

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            if (logLevel == LogLevel.Warning)
            {
                Warnings.Add(formatter(state, exception));
            }
        }
    }

    private static List<string> WarningsFor(PageSpeedOptions options)
    {
        if (options.Sidecar.Mode == SidecarMode.Inverse)
        {
            options.Sidecar.Mode = SidecarMode.Process;
        }
        var logger = new CapturingLogger<NginxConfigGenerator>();
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetPort(5000);
        var prefix = Path.Combine(Path.GetTempPath(), "psgen_" + Guid.NewGuid().ToString("N"));
        try
        {
            new NginxConfigGenerator(logger, endpoint).GenerateConfig(options, prefix);
        }
        finally
        {
            try { Directory.Delete(prefix, recursive: true); } catch { /* best effort */ }
        }
        return logger.Warnings;
    }

    [Fact]
    public void WarnUnmappedOptions_VirtualHostsConfigured_LogsWarning()
    {
        var warnings = WarningsFor(new PageSpeedOptions
        {
            VirtualHosts = [new VirtualHostOptions { HostPattern = "*.example.com" }]
        });
        warnings.Should().Contain(w => w.Contains("VirtualHosts"));
    }

    [Fact]
    public void WarnUnmappedOptions_NoVirtualHosts_NoWarning()
    {
        var warnings = WarningsFor(new PageSpeedOptions());
        warnings.Should().NotContain(w => w.Contains("VirtualHosts"));
    }
}
