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
        if (_process != null && !_process.HasExited)
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
                    await _process.WaitForExitAsync(timeoutCts.Token);
                }
                catch (OperationCanceledException)
                {
                    _logger.LogWarning("Graceful shutdown timed out, killing process");
                    _process.Kill(entireProcessTree: true);
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Error during graceful shutdown, killing process");
                try
                {
                    _process.Kill(entireProcessTree: true);
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

    private void StartProcess(string binaryPath)
    {
        var opts = _options.Value;

        var args = new List<string>
        {
            "-c", _configPath!,
            "--log-level", "info",
            "--use-dynamic-base-id"
        };

        args.AddRange(opts.Sidecar.AdditionalArguments);

        var startInfo = new ProcessStartInfo
        {
            FileName = binaryPath,
            Arguments = string.Join(" ", args),
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };

        foreach (var (key, value) in opts.Sidecar.EnvironmentVariables)
        {
            startInfo.Environment[key] = value;
        }

        _logger.LogDebug("Starting {Binary} {Args}", binaryPath, startInfo.Arguments);

        _process = new Process { StartInfo = startInfo };

        _process.OutputDataReceived += (sender, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                _logger.LogInformation("[envoy] {Data}", e.Data);
            }
        };

        _process.ErrorDataReceived += (sender, e) =>
        {
            if (!string.IsNullOrEmpty(e.Data))
            {
                // Envoy logs to stderr by default
                _logger.LogInformation("[envoy] {Data}", e.Data);
            }
        };

        if (!_process.Start())
        {
            throw new InvalidOperationException("Failed to start envoy_pagespeed process");
        }

        _process.BeginOutputReadLine();
        _process.BeginErrorReadLine();

        _logger.LogDebug("Started envoy_pagespeed with PID {Pid}", _process.Id);
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
            if (_process?.HasExited == true)
            {
                throw new InvalidOperationException(
                    $"envoy_pagespeed process exited with code {_process.ExitCode}");
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
                if (_process?.HasExited == true)
                {
                    _logger.LogWarning("Sidecar process exited unexpectedly with code {ExitCode}",
                        _process.ExitCode);
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
        try
        {
            _monitorCts?.Cancel();
        }
        catch (ObjectDisposedException)
        {
            // Already disposed
        }
        _monitorCts?.Dispose();

        if (_process != null)
        {
            if (!_process.HasExited)
            {
                try
                {
                    _process.Kill(entireProcessTree: true);
                }
                catch
                {
                    // Process may have already exited
                }
            }
            _process.Dispose();
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
