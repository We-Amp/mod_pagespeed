using System.Diagnostics;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Options;

namespace WeAmp.PageSpeed.AspNetCore.Sidecar;

/// <summary>
/// Manages the PageSpeed Envoy sidecar as a child process.
/// </summary>
public class ProcessSidecarManager : ISidecarManager, IDisposable
{
    private readonly ILogger<ProcessSidecarManager> _logger;
    private readonly IOptions<PageSpeedOptions> _options;
    private readonly EnvoyConfigGenerator _configGenerator;
    private readonly IHttpClientFactory _httpClientFactory;

    private Process? _process;
    private string? _configPath;
    private CancellationTokenSource? _monitorCts;
    private Task? _monitorTask;
    private readonly object _lock = new();

    public SidecarState State { get; private set; } = SidecarState.NotStarted;
    public string? AdminToken { get; private set; }
    public string? ErrorMessage { get; private set; }
    public int RestartCount { get; private set; }

    public event EventHandler<SidecarStateChangedEventArgs>? StateChanged;

    public ProcessSidecarManager(
        ILogger<ProcessSidecarManager> logger,
        IOptions<PageSpeedOptions> options,
        EnvoyConfigGenerator configGenerator,
        IHttpClientFactory httpClientFactory)
    {
        _logger = logger;
        _options = options;
        _configGenerator = configGenerator;
        _httpClientFactory = httpClientFactory;
    }

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

        SetState(SidecarState.Starting);

        try
        {
            // Find binary
            var binaryPath = ResolveBinaryPath();
            if (binaryPath == null)
            {
                throw new InvalidOperationException(
                    "Could not find envoy_pagespeed binary. Please install it and ensure it's in PATH, " +
                    "or set PageSpeed:Sidecar:BinaryPath in configuration.");
            }

            // Generate configuration
            var configDir = opts.Sidecar.ConfigDirectory
                ?? Path.Combine(Path.GetTempPath(), "pagespeed_sidecar");
            Directory.CreateDirectory(configDir);
            _configPath = Path.Combine(configDir, "pagespeed-envoy.yaml");

            AdminToken = _configGenerator.GenerateConfigFile(opts, _configPath);
            _logger.LogInformation("Generated configuration at {ConfigPath}", _configPath);

            // Start process
            StartProcess(binaryPath);

            // Wait for startup
            await WaitForHealthyAsync(opts.Sidecar.StartupTimeoutMs, cancellationToken);

            SetState(SidecarState.Running);
            _logger.LogInformation("PageSpeed sidecar started successfully on port {Port}",
                opts.Sidecar.ListenPort);

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
                // Send graceful shutdown via admin endpoint
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

            var healthUrl = $"http://127.0.0.1:{opts.Sidecar.ListenPort}/pagespeed/health";
            var response = await client.GetAsync(healthUrl, cancellationToken);

            return response.IsSuccessStatusCode;
        }
        catch
        {
            return false;
        }
    }

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

        // Check well-known locations
        var wellKnownLocations = new[]
        {
            "/usr/local/bin/envoy_pagespeed",
            "/opt/pagespeed/bin/envoy_pagespeed",
            Path.Combine(AppContext.BaseDirectory, "envoy_pagespeed"),
            Path.Combine(AppContext.BaseDirectory, "tools", "envoy_pagespeed"),
        };

        foreach (var path in wellKnownLocations)
        {
            if (File.Exists(path))
            {
                _logger.LogDebug("Found envoy_pagespeed at {Path}", path);
                return path;
            }
        }

        // Check PATH
        var pathEnv = Environment.GetEnvironmentVariable("PATH") ?? "";
        var pathDirs = pathEnv.Split(Path.PathSeparator);

        foreach (var dir in pathDirs)
        {
            var path = Path.Combine(dir, "envoy_pagespeed");
            if (File.Exists(path))
            {
                _logger.LogDebug("Found envoy_pagespeed in PATH at {Path}", path);
                return path;
            }
        }

        return null;
    }

    /// <summary>
    /// Builds the <see cref="ProcessStartInfo"/> for launching envoy_pagespeed.
    /// Each argument is added to <see cref="ProcessStartInfo.ArgumentList"/> so the
    /// runtime quotes/escapes every token individually; the raw
    /// <see cref="ProcessStartInfo.Arguments"/> string is never used (it performs
    /// no escaping, letting a config path with spaces or a crafted
    /// AdditionalArguments entry inject extra flags).
    /// </summary>
    internal static ProcessStartInfo BuildStartInfo(string binaryPath, string configPath, SidecarOptions sidecar)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = binaryPath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };

        // Add each token to ArgumentList so the runtime quotes/escapes them
        // individually. Never assign ProcessStartInfo.Arguments (no escaping).
        startInfo.ArgumentList.Add("-c");
        startInfo.ArgumentList.Add(configPath);
        startInfo.ArgumentList.Add("--log-level");
        startInfo.ArgumentList.Add("info");
        startInfo.ArgumentList.Add("--use-dynamic-base-id");

        foreach (var arg in sidecar.AdditionalArguments)
        {
            startInfo.ArgumentList.Add(arg);
        }

        foreach (var (key, value) in sidecar.EnvironmentVariables)
        {
            startInfo.Environment[key] = value;
        }

        return startInfo;
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

        var startInfo = BuildStartInfo(binaryPath, _configPath!, opts.Sidecar);

        _logger.LogDebug("Starting {Binary} {Args}", binaryPath,
            string.Join(" ", startInfo.ArgumentList));

        var process = new Process { StartInfo = startInfo };

        process.OutputDataReceived += (sender, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                _logger.LogInformation("[envoy] {Data}", e.Data);
            }
        };

        process.ErrorDataReceived += (sender, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                // Envoy logs to stderr by default
                _logger.LogInformation("[envoy] {Data}", e.Data);
            }
        };

        if (!process.Start())
        {
            process.Dispose();
            throw new InvalidOperationException("Failed to start envoy_pagespeed process");
        }

        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        // Publish the new process and dispose the previous one (e.g. on an
        // auto-restart) so its OS handle and stdout/stderr read pumps aren't
        // leaked once per restart.
        Process? previous;
        lock (_lock)
        {
            previous = _process;
            _process = process;
        }
        previous?.Dispose();

        _logger.LogDebug("Started envoy_pagespeed with PID {Pid}", process.Id);
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
                    $"envoy_pagespeed process exited with code {process.ExitCode}");
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
                throw new InvalidOperationException("Could not find envoy_pagespeed binary for restart");
            }

            StartProcess(binaryPath);
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
        var opts = _options.Value;

        try
        {
            using var client = _httpClientFactory.CreateClient("PageSpeedAdmin");
            client.Timeout = TimeSpan.FromSeconds(5);

            // Send quitquitquit to admin endpoint for graceful shutdown
            var adminUrl = $"http://127.0.0.1:{opts.Sidecar.AdminPort}/quitquitquit";
            await client.PostAsync(adminUrl, null, cancellationToken);
        }
        catch
        {
            // Admin endpoint may not be accessible
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

    public void Dispose()
    {
        Task? monitorTask;
        lock (_lock)
        {
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
        // leaving an orphaned envoy_pagespeed process.
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

        // Clean up config file
        if (_configPath != null && File.Exists(_configPath))
        {
            try
            {
                File.Delete(_configPath);
            }
            catch
            {
                // Ignore cleanup errors
            }
        }
    }
}
