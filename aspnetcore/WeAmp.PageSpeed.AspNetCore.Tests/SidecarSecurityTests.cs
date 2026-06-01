using FluentAssertions;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;
using YamlDotNet.Serialization;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Security-hardening regression tests for the sidecar manager and config
/// generator (see corp codebase audit 2026-05-29).
/// </summary>
public class SidecarSecurityTests
{
    private readonly EnvoyConfigGenerator _generator =
        new(NullLogger<EnvoyConfigGenerator>.Instance);

    // Walks the generated YAML to the Envoy admin interface bind address:
    // admin -> address -> socket_address -> address.
    private static string GetAdminBindAddress(string yaml)
    {
        var root = new DeserializerBuilder().Build()
            .Deserialize<Dictionary<string, object>>(yaml);
        var admin = (Dictionary<object, object>)root["admin"];
        var address = (Dictionary<object, object>)admin["address"];
        var socket = (Dictionary<object, object>)address["socket_address"];
        return (string)socket["address"];
    }

    // ---- Fix: Envoy admin interface must bind loopback by default ----
    // The admin interface is unauthenticated and exposes /quitquitquit (DoS)
    // and /config_dump (leaks the bearer token + Redis creds). Binding it to
    // all interfaces by default exposes that control plane to the network.

    [Fact]
    public void SidecarOptions_AdminBindAddress_DefaultsToLoopback()
    {
        new SidecarOptions().AdminBindAddress.Should().Be("127.0.0.1");
    }

    [Fact]
    public void GenerateConfig_DefaultOptions_BindsAdminInterfaceToLoopback()
    {
        var (yaml, _) = _generator.GenerateConfigYaml(new PageSpeedOptions());
        GetAdminBindAddress(yaml).Should().Be("127.0.0.1");
    }

    [Fact]
    public void GenerateConfig_ExplicitAdminBind_IsHonored()
    {
        var options = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { AdminBindAddress = "10.1.2.3" }
        };
        var (yaml, _) = _generator.GenerateConfigYaml(options);
        GetAdminBindAddress(yaml).Should().Be("10.1.2.3");
    }

    // ---- Fix: subprocess args must use ArgumentList, not the unescaped string ----
    // ProcessStartInfo.Arguments performs no quoting/escaping, so a config path
    // with a space or a crafted AdditionalArguments entry could inject extra
    // flags into envoy_pagespeed. ArgumentList quotes each token individually.

    [Fact]
    public void BuildStartInfo_UsesArgumentList_NotTheRawArgumentsString()
    {
        var sidecar = new SidecarOptions();
        var psi = ProcessSidecarManager.BuildStartInfo(
            "/usr/local/bin/envoy_pagespeed", "/tmp/cfg.yaml", sidecar);

        psi.Arguments.Should().BeNullOrEmpty(
            "the unescaped Arguments string must not be used");
        psi.ArgumentList.Should().ContainInOrder(
            "-c", "/tmp/cfg.yaml", "--log-level", "info", "--use-dynamic-base-id");
    }

    [Fact]
    public void BuildStartInfo_ConfigPathWithSpaces_StaysASingleToken()
    {
        var psi = ProcessSidecarManager.BuildStartInfo(
            "/usr/local/bin/envoy_pagespeed", "/tmp/with space/cfg.yaml", new SidecarOptions());

        psi.ArgumentList.Should().Contain("/tmp/with space/cfg.yaml");
    }

    [Fact]
    public void BuildStartInfo_AdditionalArguments_AreNotSplitOnSpaces()
    {
        var sidecar = new SidecarOptions
        {
            // A single argument value that contains a space must remain one token,
            // not be re-split into "--concurrency" + "4" + injected "-c" "/evil.yaml".
            AdditionalArguments = { "--concurrency 4", "-c /evil.yaml" }
        };
        var psi = ProcessSidecarManager.BuildStartInfo(
            "/usr/local/bin/envoy_pagespeed", "/tmp/cfg.yaml", sidecar);

        psi.ArgumentList.Should().Contain("--concurrency 4");
        psi.ArgumentList.Should().Contain("-c /evil.yaml");
    }

    // ---- Fix: generated config holds secrets, must not be world-readable ----
    // GenerateConfigFile serializes admin_auth.token and Redis credentials.
    // File.WriteAllText created it with the umask default (commonly 0644), so
    // any local user could read the token. Create it owner-only (0600).

    [Fact]
    public void GenerateConfigFile_WritesConfigOwnerReadableOnly()
    {
        if (OperatingSystem.IsWindows())
        {
            return; // Unix file modes only.
        }

        var dir = Path.Combine(Path.GetTempPath(), "psec_" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(dir, "pagespeed-envoy.yaml");
        try
        {
            var options = new PageSpeedOptions
            {
                Cache = new CacheOptions
                {
                    // Keep cache/log dirs inside our scratch dir so the test is self-contained.
                    FileCachePath = Path.Combine(dir, "cache"),
                    LogDirectory = Path.Combine(dir, "logs")
                }
            };

            _generator.GenerateConfigFile(options, path);

            File.Exists(path).Should().BeTrue();
            var mode = File.GetUnixFileMode(path);

            const UnixFileMode groupOther =
                UnixFileMode.GroupRead | UnixFileMode.GroupWrite | UnixFileMode.GroupExecute |
                UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute;

            (mode & groupOther).Should().Be(UnixFileMode.None,
                "the config file holds the admin token and must not be group/other readable");
            (mode & UnixFileMode.UserRead).Should().Be(UnixFileMode.UserRead);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
        }
    }

    // ---- Fix: admin bearer token must not be written to logs ----
    // StartedAsync logged the token verbatim at Information level; application
    // logs are shipped/retained/readable by more principals than should hold a
    // credential.

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

    // ---- Fix: process lifecycle / thread-safety hardening ----
    // Dispose now snapshots state under the lock and waits for the monitor
    // task. Calling it before StartAsync (no process, no monitor) must be safe
    // and idempotent.

    [Fact]
    public void Dispose_BeforeStart_IsSafeAndIdempotent()
    {
        var manager = new ProcessSidecarManager(
            NullLogger<ProcessSidecarManager>.Instance,
            Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions()),
            new EnvoyConfigGenerator(NullLogger<EnvoyConfigGenerator>.Instance),
            new StubHttpClientFactory());

        Action dispose = () => manager.Dispose();
        dispose.Should().NotThrow();
        dispose.Should().NotThrow();  // idempotent
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
