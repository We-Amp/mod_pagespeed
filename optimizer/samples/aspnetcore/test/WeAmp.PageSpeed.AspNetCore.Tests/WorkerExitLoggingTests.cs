// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// A worker that exits on its own (for example because its API port is taken)
/// used to leave no trace in the host log: the host logged "started" and then
/// waited silently, with the worker's output at Debug only. These tests drive
/// <see cref="WorkerProcessHost"/> with a fake child process and check that
/// the exit is reported.
/// </summary>
public class WorkerExitLoggingTests
{
    [Fact]
    public async Task NonZeroExit_LogsErrorWithCodeDurationAndStderrTail()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return;

        var log = new ListLogger();
        var script = WriteScript(
            "echo 'starting up' >&2\n"
            + "echo 'bind: address already in use' >&2\n"
            + "exit 3\n");
        try
        {
            await RunToExit(script, log);

            var entry = Assert.Single(log.Entries, e => e.Level == LogLevel.Error);
            Assert.Contains("exited with code 3", entry.Message);
            Assert.Contains("s.", entry.Message);
            Assert.Contains("bind: address already in use", entry.Message);
            Assert.Contains("starting up", entry.Message);
        }
        finally { File.Delete(script); }
    }

    [Fact]
    public async Task StderrTail_KeepsOnlyTheLastLines()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return;

        var log = new ListLogger();
        var script = WriteScript(
            "i=1\nwhile [ $i -le 50 ]; do echo \"line-$i\" >&2; i=$((i+1)); done\nexit 1\n");
        try
        {
            await RunToExit(script, log);

            var entry = Assert.Single(log.Entries, e => e.Level == LogLevel.Error);
            Assert.Contains("line-50", entry.Message);
            Assert.Contains("line-31", entry.Message);
            Assert.DoesNotContain("line-30\n", entry.Message + "\n");
            Assert.DoesNotContain("line-1\n", entry.Message + "\n");
        }
        finally { File.Delete(script); }
    }

    [Fact]
    public async Task CleanExit_LogsWarning()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return;

        var log = new ListLogger();
        var script = WriteScript("exit 0\n");
        try
        {
            await RunToExit(script, log);

            Assert.DoesNotContain(log.Entries, e => e.Level == LogLevel.Error);
            var entry = Assert.Single(log.Entries, e => e.Level == LogLevel.Warning
                && e.Message.Contains("exited with code 0"));
            Assert.Contains("run until shutdown", entry.Message);
        }
        finally { File.Delete(script); }
    }

    [Fact]
    public async Task ShutdownWhileRunning_DoesNotLogAnExit()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return;

        var log = new ListLogger();
        var script = WriteScript("exec sleep 30\n");
        var host = NewHost(script, log);
        try
        {
            await host.StartAsync(CancellationToken.None);
            await WaitFor(() => log.Entries.Any(e => e.Message.Contains("started")));
            await host.StopAsync(CancellationToken.None);

            Assert.DoesNotContain(log.Entries, e => e.Level >= LogLevel.Warning);
            Assert.Contains(log.Entries,
                e => e.Level == LogLevel.Information && e.Message.Contains("stopping with the host"));
        }
        finally
        {
            host.Dispose();
            File.Delete(script);
        }
    }

    private static WorkerProcessHost NewHost(string script, ILogger<WorkerProcessHost> log)
    {
        var options = new PageSpeedOptions();
        options.Worker.SocketPath = Path.Combine(Path.GetTempPath(), "wx-unused.sock");
        options.Cache.VolumePath = Path.Combine(Path.GetTempPath(), "wx-unused-cache.vol");
        return new WorkerProcessHost(
            Options.Create(options), new InternalWorkerEndpoint(), log)
        {
            WorkerPathOverride = script,
        };
    }

    private static async Task RunToExit(string script, ListLogger log)
    {
        var host = NewHost(script, log);
        try
        {
            await host.StartAsync(CancellationToken.None);
            await WaitFor(() => log.Entries.Any(e => e.Message.Contains("exited with code")));
            await host.StopAsync(CancellationToken.None);
        }
        finally { host.Dispose(); }
    }

    private static async Task WaitFor(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow.AddSeconds(30);
        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, "timed out waiting for the host log entry");
            await Task.Delay(25);
        }
    }

    private static string WriteScript(string body)
    {
        var path = Path.Combine(Path.GetTempPath(), $"fake-worker-{Guid.NewGuid():N}.sh");
        File.WriteAllText(path, "#!/bin/sh\n" + body);
        File.SetUnixFileMode(path,
            UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        return path;
    }

    private sealed class ListLogger : ILogger<WorkerProcessHost>
    {
        private readonly List<(LogLevel Level, string Message)> _entries = new();

        public IReadOnlyList<(LogLevel Level, string Message)> Entries
        {
            get { lock (_entries) return _entries.ToArray(); }
        }

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            lock (_entries) _entries.Add((logLevel, formatter(state, exception)));
        }
    }
}
