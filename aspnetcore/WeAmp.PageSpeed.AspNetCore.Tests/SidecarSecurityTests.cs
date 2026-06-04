using FluentAssertions;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Security-hardening regression tests for the nginx sidecar manager and config
/// generator (corp codebase audit 2026-05-29; the design record D8).
/// </summary>
public class SidecarSecurityTests
{
    private static NginxConfigGenerator NewGenerator()
    {
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetPort(5000);
        return new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint);
    }

    // ---- subprocess args use ArgumentList, never the unescaped string ----
    // ProcessStartInfo.Arguments performs no quoting/escaping, so a config path
    // with a space could inject extra flags. ArgumentList quotes each token.

    [Fact]
    public void BuildStartInfo_UsesArgumentList_NotTheRawArgumentsString()
    {
        var psi = ProcessSidecarManager.BuildStartInfo(
            "/opt/ps/nginx", "/opt/ps/run/nginx.conf", new PageSpeedOptions());

        psi.Arguments.Should().BeNullOrEmpty("the unescaped Arguments string must not be used");
        psi.ArgumentList.Should().ContainInOrder("-p", "/opt/ps/run", "-c", "/opt/ps/run/nginx.conf", "-g", "daemon off;");
    }

    [Fact]
    public void BuildStartInfo_ConfigPathWithSpaces_StaysASingleToken()
    {
        var psi = ProcessSidecarManager.BuildStartInfo(
            "/opt/ps/nginx", "/tmp/with space/nginx.conf", new PageSpeedOptions());

        psi.ArgumentList.Should().Contain("/tmp/with space/nginx.conf");
    }

    [Fact]
    public void BuildQuitStartInfo_UsesArgumentList_ForGracefulStop()
    {
        var psi = ProcessSidecarManager.BuildQuitStartInfo("/opt/ps/nginx", "/opt/ps/run/nginx.conf");

        psi.Arguments.Should().BeNullOrEmpty();
        psi.ArgumentList.Should().ContainInOrder("-p", "/opt/ps/run", "-c", "/opt/ps/run/nginx.conf", "-s", "quit");
    }

    // ---- generated config holds the admin bearer token, must be 0600 ----

    [Fact]
    public void GenerateConfigFile_WritesConfigOwnerReadableOnly()
    {
        if (OperatingSystem.IsWindows())
        {
            return; // Unix file modes only.
        }

        var dir = Path.Combine(Path.GetTempPath(), "psec_" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(dir, "nginx.conf");
        try
        {
            var options = new PageSpeedOptions
            {
                // Pin to Process: the default flipped to Inverse, which requires the
                // nginx loopback port pinned on the endpoint; this 0600-mode check is
                // mode-independent.
                Sidecar = new SidecarOptions { Mode = SidecarMode.Process },
                Cache = new CacheOptions
                {
                    FileCachePath = Path.Combine(dir, "cache"),
                    LogDirectory = Path.Combine(dir, "logs")
                }
            };

            NewGenerator().GenerateConfigFile(options, path);

            File.Exists(path).Should().BeTrue();
            var mode = File.GetUnixFileMode(path);

            const UnixFileMode groupOther =
                UnixFileMode.GroupRead | UnixFileMode.GroupWrite | UnixFileMode.GroupExecute |
                UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute;

            (mode & groupOther).Should().Be(UnixFileMode.None,
                "the config file embeds the admin bearer token and must not be group/other readable");
            (mode & UnixFileMode.UserRead).Should().Be(UnixFileMode.UserRead);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
        }
    }

    // ---- admin endpoints bind loopback-only by default ----
    // ngx_pagespeed has no native admin auth; the generated nginx ACL IS the gate
    //.

    [Fact]
    public void GeneratedConfig_AdminEndpoints_AreLoopbackOnlyAndBearerGated()
    {
        var prefix = Path.Combine(Path.GetTempPath(), "psec_" + Guid.NewGuid().ToString("N"));
        try
        {
            var (conf, _) = NewGenerator().GenerateConfig(
                new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Process } }, prefix);
            conf.Should().Contain("allow 127.0.0.1/32;");
            conf.Should().Contain("deny all;");
            conf.Should().Contain("if ($admin_ok = 0) { return 403; }");
        }
        finally { try { Directory.Delete(prefix, true); } catch { } }
    }

    // ---- admin bearer token must not be written to logs ----

    [Fact]
    public async Task StartedAsync_DoesNotLogTheAdminTokenValue()
    {
        const string token = "deadbeefcafef00d-supersecret-admin-bearer-token";
        var manager = new FakeSidecarManager { State = SidecarState.Running, AdminToken = token };
        var logger = new CapturingLogger<PageSpeedSidecarHostedService>();
        var options = Microsoft.Extensions.Options.Options.Create(
            new PageSpeedOptions { Enabled = true });
        var service = new PageSpeedSidecarHostedService(
            logger, manager, options, new FakeHostApplicationLifetime());

        await service.StartedAsync(CancellationToken.None);

        logger.Messages.Should().NotContain(m => m.Contains(token),
            "the admin bearer token must never be written to logs");
    }

    // ---- process lifecycle / thread-safety hardening ----

    [Fact]
    public void Dispose_BeforeStart_IsSafeAndIdempotent()
    {
        var manager = new ProcessSidecarManager(
            NullLogger<ProcessSidecarManager>.Instance,
            Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions()),
            NewGenerator(),
            new StubHttpClientFactory(),
            new InternalSidecarEndpoint());

        Action dispose = () => manager.Dispose();
        dispose.Should().NotThrow();
        dispose.Should().NotThrow();  // idempotent
    }

    [Fact]
    public async Task StopAsync_WhenNotStarted_IsSafe()
    {
        var manager = new ProcessSidecarManager(
            NullLogger<ProcessSidecarManager>.Instance,
            Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions()),
            NewGenerator(),
            new StubHttpClientFactory(),
            new InternalSidecarEndpoint());

        await manager.Invoking(m => m.StopAsync(CancellationToken.None)).Should().NotThrowAsync();
        manager.State.Should().Be(SidecarState.Stopped);
    }

    private sealed class StubHttpClientFactory : System.Net.Http.IHttpClientFactory
    {
        public System.Net.Http.HttpClient CreateClient(string name) =>
            new System.Net.Http.HttpClient();
    }

    private sealed class FakeSidecarManager : ISidecarManager
    {
        public SidecarState State { get; set; } = SidecarState.Running;
        public string? AdminToken { get; set; }
        public string? ErrorMessage { get; set; }
        public int RestartCount { get; set; }
#pragma warning disable CS0067 // event required by interface, not exercised here
        public event EventHandler<SidecarStateChangedEventArgs>? StateChanged;
#pragma warning restore CS0067
        public Task StartAsync(CancellationToken cancellationToken = default) => Task.CompletedTask;
        public Task StopAsync(CancellationToken cancellationToken = default) => Task.CompletedTask;
        public Task<bool> CheckHealthAsync(CancellationToken cancellationToken = default) => Task.FromResult(true);
    }

    private sealed class FakeHostApplicationLifetime : IHostApplicationLifetime
    {
        public CancellationToken ApplicationStarted => CancellationToken.None;
        public CancellationToken ApplicationStopping => CancellationToken.None;
        public CancellationToken ApplicationStopped => CancellationToken.None;
        public void StopApplication() { }
    }

    private sealed class CapturingLogger<T> : ILogger<T>
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
