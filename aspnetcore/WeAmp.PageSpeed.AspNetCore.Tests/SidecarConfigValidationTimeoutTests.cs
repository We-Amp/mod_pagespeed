// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Diagnostics;
using FluentAssertions;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The pre-launch <c>nginx -t</c> validation runs under a budget
/// (<see cref="SidecarOptions.ConfigValidationTimeoutMs"/>, default 60 s). When the
/// budget expires, the validation process is killed and the failure is a
/// <see cref="TimeoutException"/> carrying the elapsed time and the output captured
/// so far; a cancelled caller token also kills the child and cancels promptly; a
/// non-positive budget is rejected at startup by the options validator.
/// </summary>
public class SidecarConfigValidationTimeoutTests
{
    // ---------------- option default + startup validation ----------------

    [Fact]
    public void ConfigValidationTimeoutMs_DefaultsTo60000()
    {
        new SidecarOptions().ConfigValidationTimeoutMs.Should().Be(60_000,
            "nginx -t finishes well under a second on a healthy host, so the default only changes behaviour on a host too loaded to finish at all");
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    public void ConfigValidationTimeoutMs_NonPositive_IsRejectedAtStartup(int value)
    {
        var options = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process, ConfigValidationTimeoutMs = value },
        };

        // ValidateOnStart runs this validator at host build, so this is the startup
        // rejection an operator sees.
        var result = new PageSpeedOptionsValidator().Validate(null, options);

        result.Failed.Should().BeTrue("a non-positive budget must fail fast at startup");
        result.FailureMessage.Should().Contain("ConfigValidationTimeoutMs");
    }

    [Fact]
    public void ConfigValidationTimeoutMs_Positive_IsAccepted()
    {
        var options = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process, ConfigValidationTimeoutMs = 15_000 },
        };

        new PageSpeedOptionsValidator().Validate(null, options).Succeeded.Should().BeTrue();
    }

    // ---------------- validation timeout behaviour ----------------

    [Fact]
    public async Task ValidationExceedingBudget_KillsProcess_AndReportsElapsedAndOutput()
    {
        if (!IsPosix()) return;

        var dir = NewTempDir();
        var pidFile = Path.Combine(dir, "validated.pid");
        var script = WriteFakeNginx(dir, pidFile);
        try
        {
            // The budget runs from Process.Start, so it must give the fake binary's
            // start-up real headroom rather than race it: on a loaded runner the pid
            // file and the diagnostic can trail the spawn by seconds. 5000 ms buys
            // that headroom while the sleep 30 stall still dwarfs the budget.
            var act = () => NewManager().ValidateGeneratedConfigAsync(
                script, Path.Combine(dir, "nginx.conf"), timeoutMs: 5000, CancellationToken.None);

            var ex = (await act.Should().ThrowAsync<TimeoutException>()).Which;
            ex.Message.Should().MatchRegex(@"nginx -t did not finish within \d+(\.\d+)? s;",
                "the timeout must report the elapsed time");
            ex.Message.Should().Contain("pre-stall diagnostic",
                "output the process produced before the budget expired must be surfaced");
            ex.Message.Should().NotContain("post-stall diagnostic",
                "nothing printed after the stall belongs in the message");

            var pid = await ReadPidAsync(pidFile);
            (await ProcessIsGoneAsync(pid)).Should().BeTrue(
                "the timed-out nginx -t process must be killed, not left running");
        }
        finally
        {
            TryDeleteDir(dir);
        }
    }

    [Fact]
    public async Task ValidationExceedingBudget_CallerCancellation_CancelsPromptly()
    {
        if (!IsPosix()) return;

        var dir = NewTempDir();
        var pidFile = Path.Combine(dir, "validated.pid");
        var script = WriteFakeNginx(dir, pidFile);
        try
        {
            // No timer: cancelling on a fixed deadline could fire while the fake
            // binary is still starting up (its pid file and diagnostic trail the
            // spawn under load). The cancel below happens only once the child has
            // proven it is up and has printed.
            using var cts = new CancellationTokenSource();

            var validation = NewManager().ValidateGeneratedConfigAsync(
                script, Path.Combine(dir, "nginx.conf"), timeoutMs: 60_000, cts.Token);
            var act = () => validation;

            // The pid file is the script's very first act; once it exists the child
            // is running and the pre-stall diagnostic has been printed.
            var pid = await ReadPidAsync(pidFile);

            cts.Cancel();

            await act.Should().ThrowAsync<OperationCanceledException>(
                "a cancelled caller gets its cancellation back, not the 60 s timeout");
            (await ProcessIsGoneAsync(pid)).Should().BeTrue(
                "caller cancellation must not leave the nginx -t process running either");
        }
        finally
        {
            TryDeleteDir(dir);
        }
    }

    // ------------------------------- helpers -------------------------------

    private static bool IsPosix() => OperatingSystem.IsLinux() || OperatingSystem.IsMacOS();

    private static ProcessSidecarManager NewManager()
    {
        var endpoint = new InternalSidecarEndpoint();
        return new ProcessSidecarManager(
            NullLogger<ProcessSidecarManager>.Instance,
            Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions()),
            new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint),
            new StubHttpClientFactory(),
            endpoint);
    }

    // A stand-in for nginx that behaves like the wedged validation the fix targets:
    // record the pid, print a diagnostic, then outlive any sane budget. The line
    // after the stall proves output printed past the budget never leaks into the
    // message.
    private static string WriteFakeNginx(string dir, string pidFile)
    {
        var script = Path.Combine(dir, "fake-nginx");
        File.WriteAllText(script,
            "#!/bin/sh\n" +
            $"echo $$ > '{pidFile}'\n" +
            "echo 'pre-stall diagnostic'\n" +
            "sleep 30\n" +
            "echo 'post-stall diagnostic'\n");
        File.SetUnixFileMode(script,
            UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        return script;
    }

    // The script records its own pid as its very first act; poll briefly rather than
    // read immediately so a slow process start cannot fail the test.
    private static async Task<int> ReadPidAsync(string pidFile)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            if (File.Exists(pidFile) &&
                int.TryParse(File.ReadAllText(pidFile).Trim(), out var parsed))
            {
                // The shell creates the file before it writes the pid; an
                // empty or partial read just means "not yet".
                return parsed;
            }
            await Task.Delay(50);
        }
        // In a timeout-themed test a bare TimeoutException from this helper would
        // read like the bug under test; spell out what actually failed.
        throw new TimeoutException(
            $"fake nginx never started within 5 s (no pid file at {pidFile}); " +
            "a test-harness start-up failure, not the validation timeout under test");
    }

    // True once the pid no longer names a live process. GetProcessById only surfaces
    // live (or not-yet-reaped) processes, so ArgumentException means gone; polled
    // because the kill's reaping can trail the thrown timeout by a moment.
    private static async Task<bool> ProcessIsGoneAsync(int pid)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                using var p = Process.GetProcessById(pid);
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
        var dir = Path.Combine(Path.GetTempPath(), "psvt_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static void TryDeleteDir(string dir)
    {
        try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
    }

    private sealed class StubHttpClientFactory : System.Net.Http.IHttpClientFactory
    {
        public System.Net.Http.HttpClient CreateClient(string name) => new();
    }
}
