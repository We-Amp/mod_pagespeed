// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using FluentAssertions;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Sidecar housekeeping: the timing options are validated at startup with the
/// option named in the failure; once the sidecar is <see cref="SidecarState.Failed"/>
/// the monitor stops after one final line instead of repeating the same exit
/// warning every interval; a health probe answered by a listener without this
/// launch's token does not count as healthy; and the per-start origin-socket
/// directory under the temp root is removed on dispose. Outcome-based: every
/// assertion observes the manager's public state, the captured log lines, or
/// the filesystem; the only timing assumption is that state settles within a
/// generous bound.
/// </summary>
public class SidecarHousekeepingTests
{
    // ---------------- item 1: timing options validated at startup ----------------

    [Theory]
    [InlineData(0)]
    [InlineData(-5)]
    [InlineData(600_001)]
    public void HealthCheckIntervalMs_OutOfRange_FailsValidationNamingTheOption(int value)
    {
        TimingOptionOutOfRange_FailsValidationNamingTheOption(
            s => s.HealthCheckIntervalMs = value, "HealthCheckIntervalMs", value);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-5)]
    [InlineData(600_001)]
    public void HealthCheckTimeoutMs_OutOfRange_FailsValidationNamingTheOption(int value)
    {
        TimingOptionOutOfRange_FailsValidationNamingTheOption(
            s => s.HealthCheckTimeoutMs = value, "HealthCheckTimeoutMs", value);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-5)]
    [InlineData(600_001)]
    public void ShutdownTimeoutMs_OutOfRange_FailsValidationNamingTheOption(int value)
    {
        TimingOptionOutOfRange_FailsValidationNamingTheOption(
            s => s.ShutdownTimeoutMs = value, "ShutdownTimeoutMs", value);
    }

    [Fact]
    public void TimingOptions_AtTheTenMinuteBound_AreAccepted()
    {
        var options = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions
            {
                Mode = SidecarMode.Process,
                HealthCheckIntervalMs = 600_000,
                HealthCheckTimeoutMs = 600_000,
                ShutdownTimeoutMs = 600_000,
            },
        };

        // ValidateOnStart runs this validator at host build, so this is the
        // startup rejection an operator sees.
        new PageSpeedOptionsValidator().Validate(null, options).Succeeded
            .Should().BeTrue("ten minutes is the documented upper bound, not an error");
    }

    private static void TimingOptionOutOfRange_FailsValidationNamingTheOption(
        Action<SidecarOptions> set, string name, int value)
    {
        var sidecar = new SidecarOptions { Mode = SidecarMode.Process };
        set(sidecar);
        var options = new PageSpeedOptions { Sidecar = sidecar };

        var result = new PageSpeedOptionsValidator().Validate(null, options);

        result.Failed.Should().BeTrue(
            $"{name} = {value} turns the monitor into a busy loop or makes every probe time out");
        result.FailureMessage.Should().Contain($"PageSpeed:Sidecar:{name} {value}",
            "the failure must name the option so the operator can fix the typo");
    }

    // ---------------- item 2: Failed stops the repeated monitor logging ----------------

    [Fact]
    public async Task FailedSidecar_StopsTheMonitor_AndLogsTheFinalLineOnce()
    {
        if (!IsPosix()) return;

        using var harness = new Harness(new SidecarOptions
        {
            StartupTimeoutMs = 10_000,
            HealthCheckIntervalMs = 100,
            AutoRestart = false,
        }, crashWhenTriggered: true);
        await harness.Manager.StartAsync(CancellationToken.None);
        harness.Manager.State.Should().Be(SidecarState.Running, "the fake sidecar starts healthy");

        // Crash the child; with auto-restart off the first exit is terminal.
        File.WriteAllText(harness.CrashFile, "now");
        (await WaitUntilAsync(() => harness.Manager.State == SidecarState.Failed,
            TimeSpan.FromSeconds(10))).Should().BeTrue();

        // The final line lands on the next monitor tick after Failed.
        (await WaitUntilAsync(
            () => harness.Logger.CountOf("failed and will not be retried") > 0,
            TimeSpan.FromSeconds(5))).Should().BeTrue(
                "the monitor must say once that the sidecar is failed and will not be retried");

        var exits = harness.Logger.CountOf("exited unexpectedly");
        exits.Should().Be(1, "the exit warning is logged once, not once per interval");
        harness.Logger.CountOf("failed and will not be retried").Should().Be(1);

        // Several monitor intervals later nothing may have been logged again.
        await Task.Delay(600);
        harness.Logger.CountOf("exited unexpectedly").Should().Be(exits,
            "the monitor must stop rather than repeat the exit warning for the life of the app");
        harness.Logger.CountOf("failed and will not be retried").Should().Be(1);
    }

    // ---------------- item 3: a probe is bound to this launch ----------------

    [Fact]
    public async Task ProbeAnsweredByAListenerWithoutTheToken_IsNotHealthy()
    {
        if (!IsPosix()) return;

        using var harness = new Harness(new SidecarOptions { StartupTimeoutMs = 1500 });
        // The fixed stub body a DIFFERENT listener on the port answers with
        // (another app instance, or an orphan nginx from an earlier launch):
        // 200 OK, but not this launch's token.
        harness.Health.Body = "pagespeed-sidecar-ok\n";

        var act = () => harness.Manager.StartAsync(CancellationToken.None);

        await act.Should().ThrowAsync<TimeoutException>(
            "a 200 without this launch's probe token must not count as healthy");
        harness.Manager.State.Should().Be(SidecarState.Failed,
            "the sidecar must not report Running off a foreign listener's answer");
    }

    [Fact]
    public async Task ProbeAnsweredWithThisLaunchsToken_IsHealthy()
    {
        if (!IsPosix()) return;

        using var harness = new Harness(new SidecarOptions { StartupTimeoutMs = 10_000 });
        await harness.Manager.StartAsync(CancellationToken.None);

        harness.Manager.State.Should().Be(SidecarState.Running,
            "the default stub body carries this launch's token, so our own nginx still passes");
    }

    // ---------------- item 4: the per-start socket directory is reaped ----------------

    [Fact]
    public void Dispose_RemovesTheAutoBuiltOriginSocketDirectory()
    {
        if (!IsPosix()) return; // the auto UDS origin exists on POSIX only

        var appDir = NewAutoSocketDir(out var socketPath);
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetSocketPath(socketPath);

        NewManager(new SidecarOptions(), endpoint).Dispose();

        Directory.Exists(appDir).Should().BeFalse(
            "the per-start origin-socket directory must not outlive the manager " +
            "(otherwise one empty directory accumulates per app start)");
    }

    [Fact]
    public void Dispose_KeepsAnOperatorPinnedSocketDirectory()
    {
        if (!IsPosix()) return;

        var appDir = NewAutoSocketDir(out var socketPath);
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetSocketPath(socketPath);

        try
        {
            NewManager(new SidecarOptions { SocketPath = socketPath }, endpoint).Dispose();

            Directory.Exists(appDir).Should().BeTrue(
                "an operator-pinned SocketPath directory is theirs to keep");
        }
        finally
        {
            TryDeleteDir(appDir);
        }
    }

    // ------------------------------- helpers -------------------------------

    private static bool IsPosix() => OperatingSystem.IsLinux() || OperatingSystem.IsMacOS();

    private static ProcessSidecarManager NewManager(
        SidecarOptions sidecar, InternalSidecarEndpoint endpoint) =>
        new(
            NullLogger<ProcessSidecarManager>.Instance,
            Microsoft.Extensions.Options.Options.Create(
                new PageSpeedOptions { Enabled = true, Sidecar = sidecar }),
            new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint),
            new StubHealthFactory(),
            endpoint);

    /// <summary>
    /// A <see cref="ProcessSidecarManager"/> wired to a fake nginx script (the
    /// child-process seam) and a health endpoint whose response body the test
    /// controls (the probe seam). The endpoint is shared by the manager and the
    /// config generator, exactly as in DI, so the generated health token and
    /// the probe comparison agree.
    /// </summary>
    private sealed class Harness : IDisposable
    {
        public readonly string Dir = NewTempDir();
        public readonly string PidFile;
        public readonly string CrashFile;
        public readonly StubHealthFactory Health = new();
        public readonly CountingLogger Logger = new();
        public readonly InternalSidecarEndpoint Endpoint = new();
        public readonly ProcessSidecarManager Manager;

        public Harness(SidecarOptions sidecar, bool crashWhenTriggered = false)
        {
            PidFile = Path.Combine(Dir, "pids");
            CrashFile = Path.Combine(Dir, "crash");
            sidecar.Mode = SidecarMode.Process;
            sidecar.BinaryPath = WriteFakeNginx(Dir, PidFile, CrashFile, crashWhenTriggered);

            Endpoint.SetPort(5000);
            Manager = new ProcessSidecarManager(
                Logger,
                Microsoft.Extensions.Options.Options.Create(
                    new PageSpeedOptions { Enabled = true, Sidecar = sidecar }),
                new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, Endpoint),
                Health,
                Endpoint);

            // What our own nginx answers: the stub body plus this launch's token.
            Health.Body = $"pagespeed-sidecar-ok {Endpoint.HealthProbeToken}\n";
        }

        public void Dispose()
        {
            Manager.Dispose();
            TryDeleteDir(Dir);
        }
    }

    // A stand-in for nginx: the short-lived invocations (the -t validation, the
    // -s quit stop signal) succeed immediately; the master ("daemon off;")
    // records its pid and lives long. With crashWhenTriggered the master exits
    // with code 3 once the test creates the trigger file.
    private static string WriteFakeNginx(
        string dir, string pidFile, string crashFile, bool crashWhenTriggered)
    {
        var script = Path.Combine(dir, "fake-nginx");
        File.WriteAllText(script,
            "#!/bin/sh\n" +
            "for arg in \"$@\"; do\n" +
            "  case \"$arg\" in -t) exit 0 ;; quit) exit 0 ;; esac\n" +
            "done\n" +
            $"echo $$ >> '{pidFile}'\n" +
            (crashWhenTriggered
                ? $"while [ ! -f '{crashFile}' ]; do sleep 0.05; done\nexit 3\n"
                : "sleep 300\n"));
        if (!OperatingSystem.IsWindows())
        {
            File.SetUnixFileMode(script,
                UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        }
        return script;
    }

    private static async Task<bool> WaitUntilAsync(Func<bool> condition, TimeSpan budget)
    {
        var deadline = DateTime.UtcNow + budget;
        while (DateTime.UtcNow < deadline)
        {
            if (condition())
            {
                return true;
            }
            await Task.Delay(50);
        }
        return condition();
    }

    // Mimics the Kestrel configurator's auto-built socket layout: a private
    // ps-<pid>-<8hex> directory in the short temp root holding origin.sock.
    private static string NewAutoSocketDir(out string socketPath)
    {
        var baseDir = OperatingSystem.IsMacOS()
            ? "/tmp"
            : (Environment.GetEnvironmentVariable("TMPDIR") is { Length: > 0 } t ? t : "/tmp");
        var appDir = Path.Combine(baseDir,
            $"ps-{Environment.ProcessId}-{Guid.NewGuid().ToString("N").Substring(0, 8)}");
        ProcessSidecarManager.CreatePrivateDirectory(appDir);
        socketPath = Path.Combine(appDir, "origin.sock");
        return appDir;
    }

    private static string NewTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "pshk_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static void TryDeleteDir(string dir)
    {
        try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
    }

    // The health seam: the response body the probe will read back.
    private sealed class StubHealthFactory : System.Net.Http.IHttpClientFactory
    {
        public string Body { get; set; } = "";

        public System.Net.Http.HttpClient CreateClient(string name) =>
            new(new StubHandler(this), disposeHandler: false);

        private sealed class StubHandler(StubHealthFactory owner) : System.Net.Http.HttpMessageHandler
        {
            protected override Task<System.Net.Http.HttpResponseMessage> SendAsync(
                HttpRequestMessage request, CancellationToken cancellationToken) =>
                Task.FromResult(new System.Net.Http.HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(owner.Body),
                });
        }
    }

    private sealed class CountingLogger : ILogger<ProcessSidecarManager>
    {
        private readonly List<string> _messages = new();

        public IDisposable BeginScope<TState>(TState state) where TState : notnull =>
            NullScope.Instance;
        public bool IsEnabled(LogLevel logLevel) => true;
        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            lock (_messages) _messages.Add(formatter(state, exception));
        }

        public int CountOf(string fragment)
        {
            lock (_messages)
            {
                return _messages.Count(m => m.Contains(fragment, StringComparison.Ordinal));
            }
        }

        private sealed class NullScope : IDisposable
        {
            public static readonly NullScope Instance = new();
            public void Dispose() { }
        }
    }
}
