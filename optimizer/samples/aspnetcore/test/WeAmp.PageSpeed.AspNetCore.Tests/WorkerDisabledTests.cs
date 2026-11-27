// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// PageSpeed:Enabled false must not launch factory_worker. AutoStart false
/// still must not launch a child when Enabled stays true, so an external
/// worker (and the tests that opt out of a subprocess) keep working, and it
/// still publishes the external worker's socket path when Enabled is false.
/// Under hot reload, a host that started disabled launches the worker once
/// Enabled becomes true, and switching Enabled off leaves it running.
/// </summary>
public class WorkerDisabledTests
{
    [Fact]
    public async Task EnabledFalse_DoesNotLaunchWorker_EvenWhenAutoStartIsTrue()
    {
        using var fx = new LauncherFixture();
        var options = fx.NewOptions(enabled: false);
        var host = fx.NewHost(Options.Create(options));

        try
        {
            await host.StartAsync(CancellationToken.None);

            var socketPath = await fx.Endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Null(socketPath);
            Assert.Contains(fx.Log.Entries, e =>
                e.Level == LogLevel.Information
                && e.Message.Contains("PageSpeed is disabled"));
            Assert.DoesNotContain(fx.Log.Entries, e => e.Message.Contains("Starting factory_worker"));
            await Task.Delay(300);
            Assert.False(File.Exists(fx.Sentinel));
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
            host.Dispose();
        }
    }

    [Fact]
    public async Task AutoStartFalse_DoesNotLaunchWorker_WhenEnabled()
    {
        using var fx = new LauncherFixture();
        var options = new PageSpeedOptions { Enabled = true };
        options.Worker.AutoStart = false;
        options.Cache.VolumePath = fx.VolumePath;
        var host = fx.NewHost(Options.Create(options));

        try
        {
            await host.StartAsync(CancellationToken.None);

            var socketPath = await fx.Endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Null(socketPath);
            Assert.Contains(fx.Log.Entries, e =>
                e.Level == LogLevel.Information
                && e.Message.Contains("Worker auto-start is disabled"));
            Assert.DoesNotContain(fx.Log.Entries, e => e.Message.Contains("Starting factory_worker"));
            await Task.Delay(300);
            Assert.False(File.Exists(fx.Sentinel));
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
            host.Dispose();
        }
    }

    [Fact]
    public async Task AutoStartFalse_PublishesExternalSocketPath_EvenWhenDisabled()
    {
        using var fx = new LauncherFixture();
        var options = fx.NewOptions(enabled: false);
        options.Worker.AutoStart = false;
        var external = Path.Combine(Path.GetTempPath(), $"ps-external-{Guid.NewGuid():N}.sock");
        options.Worker.SocketPath = external;
        var host = fx.NewHost(Options.Create(options));

        try
        {
            await host.StartAsync(CancellationToken.None);

            var socketPath = await fx.Endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(external, socketPath);
            Assert.DoesNotContain(fx.Log.Entries, e => e.Message.Contains("Starting factory_worker"));
            await Task.Delay(300);
            Assert.False(File.Exists(fx.Sentinel));
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
            host.Dispose();
        }
    }

    [Fact]
    public async Task HotReload_EnabledBecomesTrue_StartsWorkerAndPublishesSocketPath()
    {
        using var fx = new LauncherFixture();
        var monitor = new TestOptionsMonitor(fx.NewOptions(enabled: false));
        var host = fx.NewHost(monitor);

        try
        {
            await host.StartAsync(CancellationToken.None);

            // Disabled: no worker, and nothing published yet, so the
            // notification service keeps waiting for the worker's socket.
            // Wait for the host to report it is waiting rather than sleeping:
            // on a loaded runner the host can take longer than a fixed delay.
            await WaitFor(() => fx.Log.Entries.Any(e =>
                    e.Level == LogLevel.Information
                    && e.Message.Contains("PageSpeed is disabled")
                    && e.Message.Contains("no restart needed")),
                "the host to report it is waiting for Enabled");
            // Short settle so a regression that launches right after the log line is still caught.
            await Task.Delay(300);
            Assert.False(fx.Endpoint.SocketPathReady.IsCompleted);
            Assert.False(File.Exists(fx.Sentinel));

            var enabled = fx.NewOptions(enabled: true);
            monitor.Set(enabled);

            var socketPath = await fx.Endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(30));
            Assert.Equal(fx.SocketPath, socketPath);
            // The API port is published after the socket path, just before
            // the launch, so wait for it rather than read it.
            Assert.True(await fx.Endpoint.Ready.WaitAsync(TimeSpan.FromSeconds(30)) > 0);
            await WaitFor(() => File.Exists(fx.Sentinel), "the worker to launch");
            Assert.Contains(fx.Log.Entries, e => e.Message.Contains("Starting factory_worker"));
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
            host.Dispose();
        }
    }

    [Fact]
    public async Task HotReload_AutoStartOffWhenEnabled_PublishesExternalSocketPathAndNeverLaunches()
    {
        using var fx = new LauncherFixture();
        var monitor = new TestOptionsMonitor(fx.NewOptions(enabled: false));
        var host = fx.NewHost(monitor);

        try
        {
            await host.StartAsync(CancellationToken.None);
            await Task.Delay(200);

            // The reload that enables PageSpeed also turns AutoStart off and
            // names an external worker: that worker is used, none launched.
            var enabled = fx.NewOptions(enabled: true);
            enabled.Worker.AutoStart = false;
            var external = Path.Combine(Path.GetTempPath(), $"ps-external-{Guid.NewGuid():N}.sock");
            enabled.Worker.SocketPath = external;
            monitor.Set(enabled);

            var socketPath = await fx.Endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.Equal(external, socketPath);
            await Task.Delay(300);
            Assert.False(File.Exists(fx.Sentinel));
            Assert.DoesNotContain(fx.Log.Entries, e => e.Message.Contains("Starting factory_worker"));
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
            host.Dispose();
        }
    }

    [Fact]
    public async Task HotReload_EnabledBecomesFalse_LeavesRunningWorkerRunning()
    {
        using var fx = new LauncherFixture();
        var monitor = new TestOptionsMonitor(fx.NewOptions(enabled: true));
        var host = fx.NewHost(monitor);

        try
        {
            await host.StartAsync(CancellationToken.None);
            await WaitFor(() => File.Exists(fx.Sentinel), "the worker to launch");
            await WaitFor(() => fx.Log.Entries.Any(e => e.Message.Contains("factory_worker started")),
                "the started log line");

            monitor.Set(fx.NewOptions(enabled: false));
            await Task.Delay(500);

            Assert.DoesNotContain(fx.Log.Entries, e => e.Message.Contains("exited"));
            Assert.DoesNotContain(fx.Log.Entries, e => e.Message.Contains("stopping with the host"));
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
            host.Dispose();
        }
        Assert.Contains(fx.Log.Entries, e => e.Message.Contains("stopping with the host"));
    }

    [Fact]
    public async Task HotReload_StopWhileDisabled_EndsWithoutLaunching()
    {
        using var fx = new LauncherFixture();
        var monitor = new TestOptionsMonitor(fx.NewOptions(enabled: false));
        var host = fx.NewHost(monitor);

        try
        {
            await host.StartAsync(CancellationToken.None);
            await Task.Delay(200);
            await host.StopAsync(CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(10));

            Assert.False(File.Exists(fx.Sentinel));
            Assert.DoesNotContain(fx.Log.Entries, e => e.Message.Contains("Starting factory_worker"));
        }
        finally
        {
            host.Dispose();
        }
    }

    private static async Task WaitFor(Func<bool> condition, string what)
    {
        var deadline = DateTime.UtcNow.AddSeconds(30);
        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, $"timed out waiting for {what}");
            await Task.Delay(25);
        }
    }

    /// <summary>
    /// A stand-in worker: a script that writes a sentinel file when it is
    /// launched and then stays up until the host stops it. A shell script
    /// with the executable bit on Linux and macOS, a batch file on Windows.
    /// </summary>
    private sealed class LauncherFixture : IDisposable
    {
        public string Sentinel { get; } =
            Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.txt");
        public string SocketPath { get; } =
            Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.sock");
        public string VolumePath { get; } =
            Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}", "cache.vol");
        public string Script { get; }
        public ListLogger Log { get; } = new();
        public InternalWorkerEndpoint Endpoint { get; } = new();

        public LauncherFixture()
        {
            Script = WriteLauncher(Sentinel);
        }

        public PageSpeedOptions NewOptions(bool enabled)
        {
            var options = new PageSpeedOptions { Enabled = enabled };
            options.Worker.SocketPath = SocketPath;
            options.Cache.VolumePath = VolumePath;
            return options;
        }

        public WorkerProcessHost NewHost(IOptions<PageSpeedOptions> options) =>
            new(options, Endpoint, Log) { WorkerPathOverride = Script };

        public WorkerProcessHost NewHost(IOptionsMonitor<PageSpeedOptions> options) =>
            new(options, Endpoint, Log) { WorkerPathOverride = Script };

        public void Dispose()
        {
            TryDelete(Script);
            TryDelete(Sentinel);
            var dir = Path.GetDirectoryName(VolumePath);
            if (dir != null && Directory.Exists(dir))
            {
                try { Directory.Delete(dir, recursive: true); }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }

        private static void TryDelete(string path)
        {
            try { File.Delete(path); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        private static string WriteLauncher(string sentinel)
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                var cmd = Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.cmd");
                File.WriteAllText(cmd,
                    $"@echo launched> \"{sentinel}\"\r\n"
                    + "@ping -n 31 127.0.0.1 > nul\r\n");
                return cmd;
            }

            var sh = Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.sh");
            File.WriteAllText(sh,
                "#!/bin/sh\n"
                + $"echo launched > '{sentinel}'\n"
                + "exec sleep 30\n");
            File.SetUnixFileMode(sh,
                UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            return sh;
        }
    }

    /// <summary>An options monitor whose value the test replaces, like a reload.</summary>
    private sealed class TestOptionsMonitor : IOptionsMonitor<PageSpeedOptions>
    {
        private readonly List<Action<PageSpeedOptions, string?>> _listeners = new();
        private PageSpeedOptions _value;

        public TestOptionsMonitor(PageSpeedOptions value) { _value = value; }

        public PageSpeedOptions CurrentValue => Volatile.Read(ref _value);
        public PageSpeedOptions Get(string? name) => CurrentValue;

        public IDisposable? OnChange(Action<PageSpeedOptions, string?> listener)
        {
            lock (_listeners) _listeners.Add(listener);
            return new Registration(this, listener);
        }

        public void Set(PageSpeedOptions value)
        {
            Volatile.Write(ref _value, value);
            Action<PageSpeedOptions, string?>[] listeners;
            lock (_listeners) listeners = _listeners.ToArray();
            foreach (var listener in listeners)
                listener(value, Options.DefaultName);
        }

        private sealed class Registration(TestOptionsMonitor owner,
            Action<PageSpeedOptions, string?> listener) : IDisposable
        {
            public void Dispose()
            {
                lock (owner._listeners) owner._listeners.Remove(listener);
            }
        }
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
