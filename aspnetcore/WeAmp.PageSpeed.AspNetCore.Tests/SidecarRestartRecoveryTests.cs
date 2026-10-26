// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Collections.Concurrent;
using System.Net;
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
/// A restart that misses its startup deadline must not park the sidecar in
/// <see cref="SidecarState.Restarting"/> (#295): a health probe that succeeds while the
/// state is Restarting promotes it to Running, a restart whose child never becomes
/// healthy is stopped (so the next monitor tick retries and the attempt counts toward
/// MaxRestartAttempts), and the hosted service stops a restarting sidecar on shutdown.
/// Outcome-based: every assertion observes the manager's public state or the fake
/// child's liveness; the only timing assumption is that recovery lands within a
/// generous bound.
/// </summary>
public class SidecarRestartRecoveryTests
{
    // ---------------- promotion: Restarting -> Running on a healthy probe ----------------

    [Fact]
    public async Task RestartingSidecar_ThatAnswersHealth_IsPromotedToRunning()
    {
        if (!IsPosix()) return;

        using var harness = NewHarness(firstStartCrashes: false);
        await harness.Manager.StartAsync(CancellationToken.None);
        harness.Manager.State.Should().Be(SidecarState.Running,
            "the fake sidecar starts healthy");

        // The state a slow restart leaves behind: Restarting with a live child that
        // now answers health probes (the startup deadline fired, recovery did not).
        harness.Manager.SetState(SidecarState.Restarting);

        var state = await WaitForStateAsync(harness.Manager, SidecarState.Running,
            TimeSpan.FromSeconds(10));

        state.Should().Be(SidecarState.Running,
            "a successful health probe during Restarting must promote the sidecar, not park it there for the life of the app");
        harness.Manager.RestartCount.Should().Be(0,
            "a restart that recovers counts as successful");
        harness.Logger.Snapshot().Should().ContainSingle(m => m.Contains("finished restarting"),
            "the promotion is logged once at Information");
    }

    // ---------------- timed-out restart: child stopped, attempt counted ----------------

    [Fact]
    public async Task RestartMissingStartupDeadline_StopsChild_AndCountsTowardMaxAttempts()
    {
        if (!IsPosix()) return;

        using var harness = NewHarness(firstStartCrashes: true, sidecar: new SidecarOptions
        {
            StartupTimeoutMs = 500,      // every restart times out while still running
            HealthCheckIntervalMs = 100,
            RestartDelayMs = 100,
            MaxRestartAttempts = 2,
        });
        await harness.Manager.StartAsync(CancellationToken.None);
        harness.Manager.State.Should().Be(SidecarState.Running);

        // From here the fake proxy never reports healthy again: the first child
        // crashes on its own, and every restarted child stays alive but unhealthy
        // past the startup deadline (exactly the #295 scenario).
        harness.Health.Healthy = false;

        var state = await WaitForStateAsync(harness.Manager, SidecarState.Failed,
            TimeSpan.FromSeconds(30));

        state.Should().Be(SidecarState.Failed,
            "a restart that misses its deadline must be stopped and retried, so the attempts reach MaxRestartAttempts instead of parking in Restarting");
        harness.Manager.ErrorMessage.Should().Contain("maximum restart attempts");
        harness.Manager.RestartCount.Should().Be(3,
            "two failed restart attempts bring the count past MaxRestartAttempts (2)");
        harness.SeenStates.Should().Contain(SidecarState.Restarting)
            .And.Contain(SidecarState.Failed);

        var pids = ReadPids(harness.PidFile);
        pids.Should().HaveCount(3,
            "the initial start plus one child per restart attempt; a timed-out child is stopped, never reused");
        foreach (var pid in pids)
        {
            (await ProcessIsGoneAsync(pid)).Should().BeTrue(
                "a restart whose startup wait timed out must stop the still-running child");
        }

        harness.Logger.Snapshot().Should().Contain(m => m.Contains("Failed to restart sidecar (attempt 1/2)"),
            "the failed-restart log line carries the attempt count");
    }

    // ---------------- shutdown stops a restarting sidecar ----------------

    [Fact]
    public async Task HostedServiceStopAsync_StopsSidecarFromRestartingState()
    {
        if (!IsPosix()) return;

        using var harness = NewHarness(firstStartCrashes: false, sidecar: new SidecarOptions
        {
            // The fake ignores nginx -s quit, so the stop falls back to the kill
            // path after this budget; keep it small so the test stays quick.
            ShutdownTimeoutMs = 1000,
        });
        await harness.Manager.StartAsync(CancellationToken.None);
        harness.Manager.State.Should().Be(SidecarState.Running);

        var child = ReadPids(harness.PidFile).Single();
        harness.Manager.SetState(SidecarState.Restarting);

        using var hosted = new PageSpeedSidecarHostedService(
            NullLogger<PageSpeedSidecarHostedService>.Instance,
            harness.Manager,
            Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions { Enabled = true }),
            new StubApplicationLifetime());
        await hosted.StopAsync(CancellationToken.None);

        harness.Manager.State.Should().Be(SidecarState.Stopped,
            "shutdown must stop a sidecar that is restarting, not skip the graceful stop");
        (await ProcessIsGoneAsync(child)).Should().BeTrue(
            "a restarting sidecar's child process must not outlive the host");
    }

    // ------------------------------- helpers -------------------------------

    private static bool IsPosix() => OperatingSystem.IsLinux() || OperatingSystem.IsMacOS();

    private static Harness NewHarness(bool firstStartCrashes, SidecarOptions? sidecar = null) =>
        new(firstStartCrashes, sidecar);

    /// <summary>
    /// A <see cref="ProcessSidecarManager"/> wired to a fake nginx script (the child
    /// process seam) and a health endpoint whose answers the test controls (the health
    /// seam): CheckHealthAsync dials this factory's client, so the probe outcome is a
    /// knob instead of a real port.
    /// </summary>
    private sealed class Harness : IDisposable
    {
        public readonly string Dir = NewTempDir();
        public readonly string PidFile;
        public readonly string Script;
        public readonly ControllableHttpClientFactory Health = new();
        public readonly InternalSidecarEndpoint Endpoint = new();
        public readonly CapturingLogger Logger = new();
        public readonly ProcessSidecarManager Manager;
        public readonly ConcurrentQueue<SidecarState> SeenStates = new();

        public Harness(bool firstStartCrashes, SidecarOptions? sidecar)
        {
            PidFile = Path.Combine(Dir, "pids");
            Script = WriteFakeNginx(Dir, PidFile, firstStartCrashes);

            sidecar ??= new SidecarOptions { StartupTimeoutMs = 5000 };
            sidecar.Mode = SidecarMode.Process;
            sidecar.BinaryPath = Script;

            var endpoint = Endpoint;
            endpoint.SetPort(5000);
            // The generated health stub embeds the endpoint's probe token and the
            // manager only counts a probe healthy when the body carries it.
            Health.Body = $"pagespeed-sidecar-ok {endpoint.HealthProbeToken}\n";
            Manager = new ProcessSidecarManager(
                Logger,
                Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions
                {
                    Enabled = true,
                    Sidecar = sidecar,
                }),
                new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint),
                Health,
                endpoint);
            Manager.StateChanged += (_, e) => SeenStates.Enqueue(e.NewState);
        }

        public void Dispose()
        {
            Manager.Dispose();
            TryDeleteDir(Dir);
        }
    }

    // A stand-in for nginx: the short-lived invocations (nginx -t validation, the
    // nginx -s quit stop signal) succeed immediately; the master ("daemon off;")
    // records its pid and lives long. With firstStartCrashes, only the very first
    // start crashes after a moment, like a proxy that dies once serving: every
    // restarted child then stays alive long enough to miss the startup deadline.
    private static string WriteFakeNginx(string dir, string pidFile, bool firstStartCrashes)
    {
        var script = Path.Combine(dir, "fake-nginx");
        File.WriteAllText(script,
            "#!/bin/sh\n" +
            "for arg in \"$@\"; do\n" +
            "  case \"$arg\" in -t) exit 0 ;; quit) exit 0 ;; esac\n" +
            "done\n" +
            $"echo $$ >> '{pidFile}'\n" +
            (firstStartCrashes
                ? $"first=$(head -n 1 '{pidFile}' | tr -d '[:space:]')\n" +
                  "if [ \"$$\" = \"$first\" ]; then sleep 1; exit 3; fi\n"
                : "") +
            "sleep 300\n");
        if (!OperatingSystem.IsWindows())
        {
            File.SetUnixFileMode(script,
                UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        }
        return script;
    }

    // Poll the manager's public state until it reaches the expected one. Returns the
    // state at the deadline otherwise, so the caller's assertion fails with the bound.
    private static async Task<SidecarState> WaitForStateAsync(
        ProcessSidecarManager manager, SidecarState expected, TimeSpan budget)
    {
        var deadline = DateTime.UtcNow + budget;
        while (DateTime.UtcNow < deadline)
        {
            if (manager.State == expected)
            {
                return expected;
            }
            await Task.Delay(50);
        }
        return manager.State;
    }

    private static int[] ReadPids(string pidFile) =>
        File.Exists(pidFile)
            ? File.ReadAllLines(pidFile).Select(int.Parse).ToArray()
            : Array.Empty<int>();

    // True once the pid no longer names a live process. GetProcessById only surfaces
    // live (or not-yet-reaped) processes, so ArgumentException means gone; polled
    // because a kill's reaping can trail the kill by a moment.
    private static async Task<bool> ProcessIsGoneAsync(int pid)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                using var p = System.Diagnostics.Process.GetProcessById(pid);
                if (p.HasExited) return true;
            }
            catch (ArgumentException)
            {
                return true;
            }
            await Task.Delay(50);
        }
        return false;
    }

    private static string NewTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "psrr_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static void TryDeleteDir(string dir)
    {
        try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
    }

    private sealed class ControllableHttpClientFactory : System.Net.Http.IHttpClientFactory
    {
        private readonly FakeHealthHandler _handler = new();

        public bool Healthy
        {
            get => _handler.Healthy;
            set => _handler.Healthy = value;
        }

        public string Body
        {
            get => _handler.Body;
            set => _handler.Body = value;
        }

        public System.Net.Http.HttpClient CreateClient(string name) =>
            new(_handler, disposeHandler: false);

        private sealed class FakeHealthHandler : System.Net.Http.HttpMessageHandler
        {
            public volatile bool Healthy = true;
            public volatile string Body = "pagespeed-sidecar-ok\n";

            protected override Task<HttpResponseMessage> SendAsync(
                HttpRequestMessage request, CancellationToken cancellationToken) =>
                Task.FromResult(new HttpResponseMessage(
                    Healthy ? HttpStatusCode.OK : HttpStatusCode.ServiceUnavailable)
                {
                    Content = new StringContent(Body),
                });
        }
    }

    private sealed class StubApplicationLifetime : IHostApplicationLifetime
    {
        public CancellationToken ApplicationStarted => CancellationToken.None;
        public CancellationToken ApplicationStopping => CancellationToken.None;
        public CancellationToken ApplicationStopped => CancellationToken.None;
        public void StopApplication() { }
    }

    private sealed class CapturingLogger : ILogger<ProcessSidecarManager>
    {
        public List<string> Messages { get; } = new();

        public IDisposable BeginScope<TState>(TState state) where TState : notnull =>
            NullScope.Instance;
        public bool IsEnabled(LogLevel logLevel) => true;
        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            lock (Messages) Messages.Add(formatter(state, exception));
        }

        public List<string> Snapshot()
        {
            lock (Messages) return Messages.ToList();
        }

        private sealed class NullScope : IDisposable
        {
            public static readonly NullScope Instance = new();
            public void Dispose() { }
        }
    }
}
