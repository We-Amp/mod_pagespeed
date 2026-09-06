// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;

namespace WeAmp.PageSpeed.AspNetCore.Sidecar;

/// <summary>
/// Manages the PageSpeed nginx sidecar as a child process.
/// </summary>
public class ProcessSidecarManager : ISidecarManager, IDisposable
{
    private readonly ILogger<ProcessSidecarManager> _logger;
    private readonly IOptions<PageSpeedOptions> _options;
    private readonly NginxConfigGenerator _configGenerator;
    private readonly IHttpClientFactory _httpClientFactory;
    private readonly InternalSidecarEndpoint _endpoint;

    private Process? _process;
    private string? _configPath;
    private string? _binaryPath;
    private string? _configDir;
    private bool _ownsConfigDir;
    private CancellationTokenSource? _monitorCts;
    private Task? _monitorTask;
    // Set true by Dispose under _lock so an in-flight monitor restart can't publish
    // (and thereby orphan) a freshly-spawned nginx after teardown began (CONC-1).
    private bool _shuttingDown;
    private readonly object _lock = new();

    // the design record UX-7 (GA): the bundled nginx is launched through the native
    // pagespeed-nginx-launch shim, which arms PR_SET_PDEATHSIG so a hard
    // SIGKILL/OOM-kill/container-hard-stop of THIS host process cannot orphan nginx
    // (it would otherwise reparent to init and keep holding its loopback port).
    // PR_SET_PDEATHSIG fires when the *launching thread* dies, so every nginx
    // (re)start's fork() MUST run on a single, long-lived OS thread that lives for the
    // manager's lifetime — never a threadpool thread that retires moments after the
    // fork (that retired-thread false-fire is exactly the bug that kept the shim
    // opt-in before GA; see SidecarOptions.UseLaunchShim). This is the same model the
    // .NET 11 runtime later shipped as ProcessStartInfo.KillOnParentExit.
    private Thread? _launchThread;
    private BlockingCollection<LaunchRequest>? _launchQueue;

    private sealed record LaunchRequest(
        ProcessStartInfo StartInfo, TaskCompletionSource<Process> Ready);

    public SidecarState State { get; private set; } = SidecarState.NotStarted;
    public string? AdminToken { get; private set; }
    public string? ErrorMessage { get; private set; }
    public int RestartCount { get; private set; }

    public event EventHandler<SidecarStateChangedEventArgs>? StateChanged;

    public ProcessSidecarManager(
        ILogger<ProcessSidecarManager> logger,
        IOptions<PageSpeedOptions> options,
        NginxConfigGenerator configGenerator,
        IHttpClientFactory httpClientFactory,
        InternalSidecarEndpoint endpoint)
    {
        _logger = logger;
        _options = options;
        _configGenerator = configGenerator;
        _httpClientFactory = httpClientFactory;
        _endpoint = endpoint;
    }

    /// <summary>
    /// The port the health poll dials. In Inverse mode nginx listens loopback-only
    /// on the (private) NginxLoopbackPort — NOT Sidecar.ListenPort, which is now
    /// the public Kestrel port — so probing ListenPort would hit Kestrel (no
    /// /pagespeed/health) and flap. In Process mode nginx is the public front-end
    /// on ListenPort. Internal for unit-testing the constructed URL without nginx.
    /// </summary>
    internal static int ResolveHealthPort(PageSpeedOptions opts, InternalSidecarEndpoint endpoint) =>
        opts.Sidecar.Mode == SidecarMode.Inverse && endpoint.NginxLoopbackPort > 0
            ? endpoint.NginxLoopbackPort
            : opts.Sidecar.ListenPort;

    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        var opts = _options.Value;

        if (!opts.Enabled)
        {
            _logger.LogInformation("PageSpeed is disabled, not starting sidecar");
            return;
        }

        if (opts.Sidecar.Mode == SidecarMode.External)
        {
            _logger.LogInformation("Using external sidecar mode, not starting process");
            SetState(SidecarState.Running);
            return;
        }

        // Docker mode is reserved for a future release. Fail CLEARLY
        // rather than falling through to the Process spawn path, where the unpinned
        // Kestrel endpoint would surface a misleading "no transport set" crash. The
        // options validator also rejects this at host build (defense in depth).
        if (opts.Sidecar.Mode == SidecarMode.Docker)
        {
            throw new NotSupportedException(
                "SidecarMode.Docker is reserved for a future release and is not implemented in this version. " +
                "Use SidecarMode.Process (bundled nginx child process) or SidecarMode.External (operator-managed nginx).");
        }

        SetState(SidecarState.Starting);

        try
        {
            // Find binary
            var binaryPath = ResolveBinaryPath();
            if (binaryPath == null)
            {
                throw new InvalidOperationException(
                    "Could not find the bundled nginx binary. Reference the " +
                    "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux package AND publish with a Linux RID " +
                    "(`dotnet publish -r linux-x64` or `-r linux-arm64`) so the matched nginx is copied " +
                    "to the app directory, or set PageSpeed:Sidecar:BinaryPath explicitly.");
            }
            _binaryPath = binaryPath;

            // Generate configuration into a private, per-app directory (0700) so
            // the generated conf (which embeds the admin bearer token) is never
            // readable by other local users (D8).
            var configDir = ResolveConfigDirectory(opts.Sidecar);
            _configPath = Path.Combine(configDir, "nginx.conf");

            AdminToken = _configGenerator.GenerateConfigFile(opts, _configPath, binaryPath);
            _logger.LogInformation("Generated configuration at {ConfigPath}", _configPath);

            // Surface a missing matched module early and clearly. The .so must sit
            // next to the RESOLVED nginx (not an unrelated AppContext.BaseDirectory):
            // in the runtimes/<rid>/native/ layout the binary and module live in the
            // same dir, and the generator now derives the module path from binaryPath.
            var modulePath = NginxConfigGenerator.ResolveModulePath(opts, binaryPath);
            if (!File.Exists(modulePath))
            {
                _logger.LogWarning(
                    "Matched ngx_pagespeed module not found at {ModulePath} (resolved next to {Binary}); " +
                    "nginx -t will reject the config if it cannot load it.", modulePath, binaryPath);
            }

            // Validate the generated config (nginx -t) BEFORE launching, so a bad
            // config surfaces as a clear error carrying nginx's own stderr rather than
            // an opaque "process exited with code N".
            await ValidateGeneratedConfigAsync(binaryPath, _configPath, cancellationToken);

            // Start process
            StartProcess(binaryPath);

            // Wait for startup
            await WaitForHealthyAsync(opts.Sidecar.StartupTimeoutMs, cancellationToken);

            SetState(SidecarState.Running);
            _logger.LogInformation("PageSpeed sidecar started successfully on port {Port}",
                ResolveHealthPort(opts, _endpoint));

            // Start monitoring
            StartMonitoring();
        }
        catch (Exception ex)
        {
            ErrorMessage = ex.Message;
            SetState(SidecarState.Failed);
            _logger.LogError(ex, "Failed to start PageSpeed sidecar");
            throw;
        }
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        _logger.LogInformation("Stopping PageSpeed sidecar");

        // Stop monitoring
        if (_monitorCts != null)
        {
            await _monitorCts.CancelAsync();
            if (_monitorTask != null)
            {
                try
                {
                    await _monitorTask;
                }
                catch (OperationCanceledException)
                {
                    // Expected
                }
            }
        }

        // Stop process
        var process = CurrentProcess();
        if (process != null && !process.HasExited)
        {
            var opts = _options.Value;
            try
            {
                // Send graceful shutdown (nginx -s quit)
                await SendGracefulShutdownAsync(cancellationToken);

                // Wait for graceful shutdown
                using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                timeoutCts.CancelAfter(opts.Sidecar.ShutdownTimeoutMs);

                try
                {
                    await process.WaitForExitAsync(timeoutCts.Token);
                }
                catch (OperationCanceledException)
                {
                    _logger.LogWarning("Graceful shutdown timed out, killing process");
                    process.Kill(entireProcessTree: true);
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Error during graceful shutdown, killing process");
                try
                {
                    process.Kill(entireProcessTree: true);
                }
                catch
                {
                    // Process may have already exited
                }
            }
        }

        SetState(SidecarState.Stopped);
        _logger.LogInformation("PageSpeed sidecar stopped");
    }

    public async Task<bool> CheckHealthAsync(CancellationToken cancellationToken = default)
    {
        var opts = _options.Value;

        try
        {
            using var client = _httpClientFactory.CreateClient("PageSpeedHealth");
            client.Timeout = TimeSpan.FromMilliseconds(opts.Sidecar.HealthCheckTimeoutMs);

            var healthPort = ResolveHealthPort(opts, _endpoint);
            var healthUrl = $"http://127.0.0.1:{healthPort}/pagespeed/health";
            var response = await client.GetAsync(healthUrl, cancellationToken);

            return response.IsSuccessStatusCode;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Maps the running CPU architecture to the NuGet runtime identifier the matched
    /// nginx pair is published under (<c>runtimes/&lt;rid&gt;/native/</c>), so the bundled
    /// binary resolves on both linux-x64 and linux-arm64. Using
    /// <see cref="RuntimeInformation.ProcessArchitecture"/> keeps the probe correct even
    /// when the app is published portable/RID-less; an unrecognized architecture falls
    /// back to the publish RID.
    /// </summary>
    internal static string ResolveBundledRid(Architecture arch) => arch switch
    {
        Architecture.Arm64 => "linux-arm64",
        Architecture.X64 => "linux-x64",
        _ => RuntimeInformation.RuntimeIdentifier,
    };

    private string? ResolveBinaryPath()
    {
        var opts = _options.Value;

        // Check configured path
        if (!string.IsNullOrEmpty(opts.Sidecar.BinaryPath))
        {
            if (File.Exists(opts.Sidecar.BinaryPath))
            {
                return opts.Sidecar.BinaryPath;
            }
            _logger.LogWarning("Configured binary path does not exist: {Path}", opts.Sidecar.BinaryPath);
        }

        // Check well-known locations. Bundled matched pair FIRST:
        // runtimes/{rid}/native/ is copied to AppContext.BaseDirectory. The RID is
        // derived from the running architecture (UX-6) so the matched nginx resolves on
        // both linux-x64 and linux-arm64 — a framework-dependent app keeps the binary
        // under runtimes/<rid>/native/ rather than flattening it to BaseDirectory.
        var rid = ResolveBundledRid(RuntimeInformation.ProcessArchitecture);
        var wellKnownLocations = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "nginx"),
            Path.Combine(AppContext.BaseDirectory, "runtimes", rid, "native", "nginx"),
            "/usr/local/bin/nginx",
            "/usr/sbin/nginx",
        };

        foreach (var path in wellKnownLocations)
        {
            if (File.Exists(path))
            {
                _logger.LogDebug("Found nginx at {Path}", path);
                return path;
            }
        }

        // Check PATH
        var pathEnv = Environment.GetEnvironmentVariable("PATH") ?? "";
        var pathDirs = pathEnv.Split(Path.PathSeparator);

        foreach (var dir in pathDirs)
        {
            var path = Path.Combine(dir, "nginx");
            if (File.Exists(path))
            {
                _logger.LogDebug("Found nginx in PATH at {Path}", path);
                return path;
            }
        }

        return null;
    }

    /// <summary>
    /// Resolves the private directory that holds the generated nginx.conf, the
    /// embedded-token config, and (by default) the cache/log tree. When the
    /// operator pins <see cref="SidecarOptions.ConfigDirectory"/> it is used
    /// as-is; otherwise a per-app, per-pid directory is created under the system
    /// temp root. Either way it is chmod 0700 so the bearer token is not readable
    /// by other local users (D8) — never the world-traversable shared
    /// <c>Path.GetTempPath()/pagespeed_sidecar</c> the prototype used.
    /// </summary>
    private string ResolveConfigDirectory(SidecarOptions sidecar)
    {
        var dir = sidecar.ConfigDirectory
            ?? Path.Combine(Path.GetTempPath(), $"ps-sidecar-{Environment.ProcessId}-{Guid.NewGuid():N}".Substring(0, 24));
        // Track ownership so Dispose only reaps the directory the sidecar created
        // itself (which holds the bearer-token conf). An operator-pinned
        // ConfigDirectory is left intact (their cache persists).
        _configDir = dir;
        _ownsConfigDir = sidecar.ConfigDirectory == null;
        Directory.CreateDirectory(dir);
        if (!OperatingSystem.IsWindows())
        {
            try
            {
                File.SetUnixFileMode(dir,
                    UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            }
            catch
            {
                // best-effort
            }
        }
        return dir;
    }

    /// <summary>
    /// Builds the <see cref="ProcessStartInfo"/> for launching the bundled nginx:
    /// <c>nginx -p &lt;prefix&gt; -c &lt;conf&gt; -g "daemon off;"</c>. Each argument is
    /// added to <see cref="ProcessStartInfo.ArgumentList"/> so the runtime
    /// quotes/escapes every token individually; the raw
    /// <see cref="ProcessStartInfo.Arguments"/> string is never used (no escaping).
    /// <c>daemon off;</c> keeps nginx in the foreground so the managed
    /// <see cref="Process"/> tracks the master directly.
    /// </summary>
    internal static ProcessStartInfo BuildStartInfo(
        string binaryPath, string configPath, PageSpeedOptions opts)
    {
        // the design record UX-7: a tiny native launch shim (`pagespeed-nginx-launch`, bundled
        // next to nginx) sets PR_SET_PDEATHSIG before exec'ing nginx so a hard
        // SIGKILL/OOM-kill/container-hard-stop of the host process can't orphan nginx.
        // ON by default (Sidecar.UseLaunchShim): the fork runs on a single long-lived
        // launch thread (StartProcess/_launchThread), so PR_SET_PDEATHSIG — which
        // tracks the *launching thread* — fires only on real host-process death, not
        // when a threadpool thread retires (the earlier threadpool-launch model tripped
        // that and SIGTERM'd nginx ~1s after start; fixed). Graceful teardown via
        // StopAsync (nginx -s quit) / Dispose Kill(entireProcessTree) covers normal
        // shutdown regardless. The shim execs its argv[1..], so nginx's path is the
        // first argument; Linux-only and only when the shim binary is present.
        var launcher = Path.Combine(Path.GetDirectoryName(binaryPath)!, "pagespeed-nginx-launch");
        var useShim = opts.Sidecar.UseLaunchShim && OperatingSystem.IsLinux() && File.Exists(launcher);

        var startInfo = new ProcessStartInfo
        {
            FileName = useShim ? launcher : binaryPath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };

        if (useShim)
        {
            // The shim treats argv[1] as the program to exec and argv[2..] as its args.
            startInfo.ArgumentList.Add(binaryPath);
        }

        var prefix = Path.GetDirectoryName(Path.GetFullPath(configPath))!;
        startInfo.ArgumentList.Add("-p");
        startInfo.ArgumentList.Add(prefix);
        startInfo.ArgumentList.Add("-c");
        startInfo.ArgumentList.Add(configPath);
        startInfo.ArgumentList.Add("-g");
        startInfo.ArgumentList.Add("daemon off;");

        foreach (var (key, value) in BuildChildEnvironment(opts))
        {
            startInfo.Environment[key] = value;
        }

        return startInfo;
    }

    /// <summary>
    /// Builds the environment variables applied to the nginx child process: exactly
    /// the operator-supplied <see cref="SidecarOptions.EnvironmentVariables"/> escape
    /// hatch — the package adds no entries of its own. There is no shell, so values
    /// reach the child via the process environment dictionary directly (no
    /// shell-injection surface).
    /// </summary>
    internal static Dictionary<string, string> BuildChildEnvironment(PageSpeedOptions opts)
    {
        var env = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var (key, value) in opts.Sidecar.EnvironmentVariables)
        {
            env[key] = value;
        }

        return env;
    }

    /// <summary>
    /// Builds the <c>nginx -p &lt;prefix&gt; -c &lt;conf&gt; -s quit</c> graceful-stop
    /// invocation. Uses <see cref="ProcessStartInfo.ArgumentList"/> (never the raw
    /// <see cref="ProcessStartInfo.Arguments"/> string) so no token is re-split or
    /// injected. The signalling process reads the master PID from the conf's
    /// <c>pid</c> directive and sends it SIGQUIT (graceful drain).
    /// </summary>
    internal static ProcessStartInfo BuildQuitStartInfo(string binaryPath, string configPath)
    {
        var prefix = Path.GetDirectoryName(Path.GetFullPath(configPath))!;
        var si = new ProcessStartInfo
        {
            FileName = binaryPath,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        si.ArgumentList.Add("-p");
        si.ArgumentList.Add(prefix);
        si.ArgumentList.Add("-c");
        si.ArgumentList.Add(configPath);
        si.ArgumentList.Add("-s");
        si.ArgumentList.Add("quit");
        return si;
    }

    /// <summary>
    /// Builds the <c>nginx -p &lt;prefix&gt; -c &lt;conf&gt; -t</c> config-test invocation.
    /// Uses <see cref="ProcessStartInfo.ArgumentList"/> (never the raw
    /// <see cref="ProcessStartInfo.Arguments"/> string), redirecting stdout/stderr so
    /// the caller can capture nginx's own diagnostics on failure.
    /// </summary>
    internal static ProcessStartInfo BuildConfigTestStartInfo(string binaryPath, string configPath)
    {
        var prefix = Path.GetDirectoryName(Path.GetFullPath(configPath))!;
        var si = new ProcessStartInfo
        {
            FileName = binaryPath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };
        si.ArgumentList.Add("-p");
        si.ArgumentList.Add(prefix);
        si.ArgumentList.Add("-c");
        si.ArgumentList.Add(configPath);
        si.ArgumentList.Add("-t");
        return si;
    }

    /// <summary>
    /// Runs <c>nginx -t</c> against the generated config and throws an
    /// <see cref="InvalidOperationException"/> carrying nginx's captured stderr when
    /// the test fails (non-zero exit). the design record D6 / Constraints require this to pass
    /// before the sidecar reaches <see cref="SidecarState.Running"/>, so a malformed
    /// generated config is reported with a precise diagnostic instead of an opaque
    /// "process exited" surfaced later by the health wait.
    /// </summary>
    private async Task ValidateGeneratedConfigAsync(
        string binaryPath, string configPath, CancellationToken cancellationToken)
    {
        var output = new System.Text.StringBuilder();
        using var proc = new Process { StartInfo = BuildConfigTestStartInfo(binaryPath, configPath) };
        proc.OutputDataReceived += (_, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };

        if (!proc.Start())
        {
            proc.Dispose();
            throw new InvalidOperationException("Failed to launch nginx to validate the generated config (nginx -t).");
        }
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(TimeSpan.FromSeconds(15));
        await proc.WaitForExitAsync(timeoutCts.Token);

        if (proc.ExitCode != 0)
        {
            string captured;
            lock (output) captured = output.ToString().Trim();
            throw new InvalidOperationException(
                $"Generated nginx configuration failed validation (nginx -t exited {proc.ExitCode}):" +
                (captured.Length > 0 ? $"\n{captured}" : " (no diagnostic output)"));
        }
    }

    // Returns the current process under the lock. _process is written by
    // StartProcess (from StartAsync and the monitor's restart path) and read
    // from several threads, so all access is synchronized via _lock; callers
    // snapshot once into a local and use that.
    private Process? CurrentProcess()
    {
        lock (_lock)
        {
            return _process;
        }
    }

    private void StartProcess(string binaryPath)
    {
        var opts = _options.Value;

        var startInfo = BuildStartInfo(binaryPath, _configPath!, opts);

        _logger.LogDebug("Starting {Binary} {Args}", binaryPath,
            string.Join(" ", startInfo.ArgumentList));

        // Fork nginx on the single long-lived launch thread (see _launchThread) so the
        // shim's PR_SET_PDEATHSIG parent-thread lives for the manager's lifetime. Block
        // for the started Process; GetResult() rethrows any start failure ON THIS
        // thread, preserving the synchronous-throw contract StartAsync's try/catch
        // (-> SetState(Failed) + rethrow) and WaitForHealthyAsync depend on.
        EnsureLaunchThread();
        var ready = new TaskCompletionSource<Process>(TaskCreationOptions.RunContinuationsAsynchronously);
        _launchQueue!.Add(new LaunchRequest(startInfo, ready));
        var process = ready.Task.GetAwaiter().GetResult();

        // Publish the new process and dispose the previous one (e.g. on an
        // auto-restart) so its OS handle and stdout/stderr read pumps aren't
        // leaked once per restart. If Dispose has begun tearing down (set under the
        // same lock) we must NOT publish: Dispose already snapshotted+killed _process,
        // so a process published now would be orphaned. Kill the just-spawned nginx
        // instead and leave _process untouched (CONC-1).
        Process? previous;
        bool abort;
        lock (_lock)
        {
            abort = _shuttingDown;
            if (abort)
            {
                previous = null;
            }
            else
            {
                previous = _process;
                _process = process;
            }
        }
        if (abort)
        {
            _logger.LogDebug("Sidecar is shutting down; killing the nginx just spawned for restart (not publishing).");
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            catch { /* may have already exited */ }
            process.Dispose();
            return;
        }
        previous?.Dispose();

        _logger.LogDebug("Started nginx with PID {Pid}", process.Id);
    }

    // Lazily start the single long-lived launch thread. Every nginx fork() runs on it
    // (LaunchNginxProcess) so the shim's PR_SET_PDEATHSIG tracks a thread that lives
    // for the manager's lifetime, never a retiring threadpool thread. Idempotent and
    // only ever called from StartProcess (StartAsync, then the serialized
    // monitor-restart path), so no extra locking is required.
    private void EnsureLaunchThread()
    {
        if (_launchThread != null)
        {
            return;
        }
        _launchQueue = new BlockingCollection<LaunchRequest>();
        _launchThread = new Thread(LaunchThreadLoop)
        {
            IsBackground = true,
            Name = "pagespeed-nginx-launch",
        };
        _launchThread.Start();
    }

    // Body of the launch thread: park on the queue between forks (keeping the OS
    // thread — the PR_SET_PDEATHSIG parent — alive), fork each requested nginx on this
    // thread, and hand the started Process (or the start exception) back to the caller
    // via the request's TaskCompletionSource. Exits when Dispose completes the queue
    // (nginx already killed by then). The thread parks in GetConsumingEnumerable, never
    // inside a Process operation, so process-exit teardown finds it idle.
    private void LaunchThreadLoop()
    {
        try
        {
            foreach (var req in _launchQueue!.GetConsumingEnumerable())
            {
                try
                {
                    req.Ready.TrySetResult(LaunchNginxProcess(req.StartInfo));
                }
                catch (Exception ex)
                {
                    req.Ready.TrySetException(ex);
                }
            }
        }
        catch (ObjectDisposedException) { /* queue disposed during teardown */ }
        catch (InvalidOperationException) { /* CompleteAdding raced GetConsumingEnumerable */ }
    }

    // Runs ON the dedicated launch thread: process.Start() performs the fork()/exec
    // here so the shim arms PR_SET_PDEATHSIG against this long-lived thread. Returns
    // the started Process; throws on a failed start (surfaced to the caller via the
    // TaskCompletionSource). The OutputDataReceived/ErrorDataReceived pumps run on the
    // threadpool as usual — only the fork is pinned to this thread.
    private Process LaunchNginxProcess(ProcessStartInfo startInfo)
    {
        var process = new Process { StartInfo = startInfo };

        process.OutputDataReceived += (sender, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                _logger.LogInformation("[nginx] {Data}", e.Data);
            }
        };

        process.ErrorDataReceived += (sender, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                // nginx logs to stderr by default
                _logger.LogInformation("[nginx] {Data}", e.Data);
            }
        };

        if (!process.Start())
        {
            process.Dispose();
            throw new InvalidOperationException("Failed to start nginx process");
        }

        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        return process;
    }

    private async Task WaitForHealthyAsync(int timeoutMs, CancellationToken cancellationToken)
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(timeoutMs);

        var startTime = DateTime.UtcNow;
        var retryDelay = 100;
        const int maxRetryDelay = 2000;

        while (!timeoutCts.Token.IsCancellationRequested)
        {
            var process = CurrentProcess();
            if (process?.HasExited == true)
            {
                throw new InvalidOperationException(
                    $"nginx process exited with code {process.ExitCode}");
            }

            if (await CheckHealthAsync(timeoutCts.Token))
            {
                var elapsed = DateTime.UtcNow - startTime;
                _logger.LogDebug("Sidecar became healthy after {Elapsed}ms", elapsed.TotalMilliseconds);
                return;
            }

            await Task.Delay(retryDelay, timeoutCts.Token);
            retryDelay = Math.Min(retryDelay * 2, maxRetryDelay);
        }

        throw new TimeoutException("Timed out waiting for sidecar to become healthy");
    }

    private void StartMonitoring()
    {
        _monitorCts = new CancellationTokenSource();
        _monitorTask = MonitorAsync(_monitorCts.Token);
    }

    private async Task MonitorAsync(CancellationToken cancellationToken)
    {
        var opts = _options.Value;

        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(opts.Sidecar.HealthCheckIntervalMs, cancellationToken);

                // Check if process is still running
                var process = CurrentProcess();
                if (process?.HasExited == true)
                {
                    _logger.LogWarning("Sidecar process exited unexpectedly with code {ExitCode}",
                        process.ExitCode);
                    await HandleProcessExitAsync(cancellationToken);
                    continue;
                }

                // Check health
                var healthy = await CheckHealthAsync(cancellationToken);
                if (healthy && State == SidecarState.Unhealthy)
                {
                    SetState(SidecarState.Running);
                    _logger.LogInformation("Sidecar recovered and is healthy");
                }
                else if (!healthy && State == SidecarState.Running)
                {
                    SetState(SidecarState.Unhealthy);
                    _logger.LogWarning("Sidecar health check failed");
                }
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Error monitoring sidecar");
            }
        }
    }

    private async Task HandleProcessExitAsync(CancellationToken cancellationToken)
    {
        var opts = _options.Value;

        if (!opts.Sidecar.AutoRestart)
        {
            ErrorMessage = "Sidecar process exited and auto-restart is disabled";
            SetState(SidecarState.Failed);
            return;
        }

        RestartCount++;

        if (RestartCount > opts.Sidecar.MaxRestartAttempts)
        {
            ErrorMessage = $"Exceeded maximum restart attempts ({opts.Sidecar.MaxRestartAttempts})";
            SetState(SidecarState.Failed);
            _logger.LogError("Sidecar exceeded maximum restart attempts");
            return;
        }

        SetState(SidecarState.Restarting);
        _logger.LogInformation("Restarting sidecar (attempt {Count}/{Max})",
            RestartCount, opts.Sidecar.MaxRestartAttempts);

        await Task.Delay(opts.Sidecar.RestartDelayMs, cancellationToken);

        try
        {
            var binaryPath = ResolveBinaryPath();
            if (binaryPath == null)
            {
                throw new InvalidOperationException("Could not find the nginx binary for restart");
            }

            // Re-check shutdown immediately before (re)launching: StopAsync/Dispose
            // cancel this token, and we must not spawn a child that would outlive the
            // manager. The _shuttingDown guard in StartProcess closes the residual
            // window between this check and the locked publish (CONC-1).
            if (cancellationToken.IsCancellationRequested)
            {
                return;
            }

            StartProcess(binaryPath);

            // If shutdown raced in after the check, StartProcess already aborted+killed
            // the child; don't wait on health for a process that was never published.
            if (cancellationToken.IsCancellationRequested)
            {
                return;
            }

            await WaitForHealthyAsync(opts.Sidecar.StartupTimeoutMs, cancellationToken);

            SetState(SidecarState.Running);
            _logger.LogInformation("Sidecar restarted successfully");

            // Reset restart count on successful restart
            RestartCount = 0;
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "Failed to restart sidecar");
            // Will retry on next monitor cycle
        }
    }

    private async Task SendGracefulShutdownAsync(CancellationToken cancellationToken)
    {
        // nginx graceful stop is a SECOND short-lived process, NOT an HTTP POST:
        //   nginx -p <prefix> -c <conf> -s quit     
        // It reads the master PID from the conf's `pid` directive and signals it.
        if (_binaryPath == null || _configPath == null)
        {
            return;
        }

        try
        {
            using var quit = Process.Start(BuildQuitStartInfo(_binaryPath, _configPath));
            if (quit != null)
            {
                using var to = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                to.CancelAfter(TimeSpan.FromSeconds(5));
                try { await quit.WaitForExitAsync(to.Token); }
                catch (OperationCanceledException) { /* fall through to the kill path in StopAsync */ }
            }
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "nginx -s quit failed; StopAsync will fall back to killing the process");
        }
    }

    private void SetState(SidecarState newState)
    {
        lock (_lock)
        {
            if (State == newState) return;

            var previousState = State;
            State = newState;

            StateChanged?.Invoke(this, new SidecarStateChangedEventArgs(
                previousState, newState, newState == SidecarState.Failed ? ErrorMessage : null));
        }
    }

    /// <summary>
    /// Best-effort teardown of the generated artifacts. When the sidecar created its
    /// OWN private per-app directory (<paramref name="ownsConfigDir"/>) the whole tree
    /// is removed — it embeds the admin bearer token (nginx.conf): leaving it behind
    /// leaks the secret into the temp root and accumulates one copy per process
    /// recycle. When the operator PINNED ConfigDirectory only the generated nginx.conf
    /// is removed — the directory and whatever else the operator keeps in it (cache,
    /// their own files) are theirs to keep.
    /// Extracted as an internal static so the cleanup contract is unit-testable.
    /// </summary>
    internal static void CleanupArtifacts(string? configDir, bool ownsConfigDir, string? configPath)
    {
        try
        {
            if (ownsConfigDir && configDir != null && Directory.Exists(configDir))
            {
                Directory.Delete(configDir, recursive: true);
                return;
            }
            if (configPath != null && File.Exists(configPath))
            {
                File.Delete(configPath);
            }
        }
        catch
        {
            // best-effort cleanup
        }
    }

    public void Dispose()
    {
        Task? monitorTask;
        lock (_lock)
        {
            // Mark teardown BEFORE cancelling the monitor so that if the monitor's
            // restart path slips past its cancellation re-check, StartProcess sees
            // this under _lock and refuses to publish (CONC-1).
            _shuttingDown = true;
            monitorTask = _monitorTask;
        }

        try
        {
            _monitorCts?.Cancel();
        }
        catch (ObjectDisposedException)
        {
            // Already disposed
        }

        // Wait for the monitor loop to observe cancellation and unwind before
        // tearing down the process. Otherwise it can relaunch the child (via
        // HandleProcessExitAsync) or touch a Process we're about to dispose,
        // leaving an orphaned nginx process.
        if (monitorTask != null)
        {
            try
            {
                monitorTask.Wait(TimeSpan.FromSeconds(5));
            }
            catch (AggregateException)
            {
                // OperationCanceledException from the cancelled loop is expected.
            }
        }

        _monitorCts?.Dispose();

        Process? process;
        lock (_lock)
        {
            process = _process;
            _process = null;
        }

        if (process != null)
        {
            if (!process.HasExited)
            {
                try
                {
                    process.Kill(entireProcessTree: true);
                }
                catch
                {
                    // Process may have already exited
                }
            }
            process.Dispose();
        }

        // Stop the single launch thread (it parks on the queue between forks). nginx is
        // already killed above, so the shim's PR_SET_PDEATHSIG firing on this thread's
        // exit is moot; IsBackground also tears it down at process exit as a backstop.
        // Join before disposing so the still-running GetConsumingEnumerable isn't pulled
        // out from under the thread, then dispose the collection's internal wait handle.
        try
        {
            _launchQueue?.CompleteAdding();
            _launchThread?.Join(TimeSpan.FromSeconds(2));
            _launchQueue?.Dispose();
        }
        catch (ObjectDisposedException) { /* already torn down */ }

        // Reap the generated artifacts (the secret-bearing per-app dir when we own it,
        // else just the generated nginx.conf). See CleanupArtifacts (SEC hygiene).
        CleanupArtifacts(_configDir, _ownsConfigDir, _configPath);
    }
}
