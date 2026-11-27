// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Xunit;
using Xunit.Abstractions;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The load-bearing regression gate for the v2.0.14 "fix the trial
/// experience" release: PROVES that MPS 2.0 image variants are actually
/// SERVED via content negotiation on the ORIGINAL URL.
///
/// MPS 2.0 does NOT rewrite URLs. The worker writes optimized variants
/// (WebP/AVIF) into the shared cache, and the middleware serves them at
/// the original URL by matching the request's capability mask (derived
/// from <c>Accept</c>) against the best cached alternate, advertising
/// <c>Vary: Accept, Save-Data, User-Agent</c>. So:
///
/// <list type="bullet">
/// <item><c>GET /test-image.jpg</c> with <c>Accept: image/jpeg</c> returns
/// the original JPEG bytes (passthrough).</item>
/// <item><c>GET /test-image.jpg</c> with <c>Accept: image/webp,image/avif</c>
/// returns a materially SMALLER WebP/AVIF body once the worker has written
/// the variant.</item>
/// </list>
///
/// If both Accept variants return the same JPEG regardless, the
/// optimization pipeline is broken at some layer (classification →
/// notification → worker transcode → cache write → cache ReadBest → serve)
/// and this test fails, pinning the regression.
///
/// This is a FULL-STACK test: it boots a TestServer with the real
/// PageSpeed middleware + the real native cache (P/Invoke into
/// libpagespeed) + a REAL <c>factory_worker</c> subprocess wired over the
/// same Unix-domain socket the production middleware uses. It spawns the
/// worker subprocess directly, as a deployment that runs the worker as a
/// separate process does (<c>AutoStart</c> off, a concrete <c>SocketPath</c>).
///
/// Inputs (without them the test is reported as skipped, or fails when
/// <c>PAGESPEED_E2E_REQUIRE_NATIVE=1</c>; see <see cref="ResolveNativeDir"/>):
/// <list type="bullet">
/// <item><c>PAGESPEED_E2E_NATIVE_DIR</c>: directory containing both
/// <c>factory_worker</c>(.exe) and the matching <c>libpagespeed</c> shared
/// library. Falls back to the test-output <c>runtimes/&lt;rid&gt;/native/</c>
/// layout, then to the restored NuGet cache for the loaded
/// <c>WeAmp.PageSpeed.NativeAssets.*</c> version.</item>
/// </list>
/// </summary>
[Collection(WorkerSubprocessCollection.Name)]
public sealed class ImageVariantContentNegotiationTests
{
    private readonly ITestOutputHelper _output;

    public ImageVariantContentNegotiationTests(ITestOutputHelper output)
    {
        _output = output;
    }

    [ImageVariantFact]
    public async Task WebpAvifAccept_ServesSmallerVariant_JpegAccept_ServesOriginal()
    {
        var nativeDir = ResolveNativeDir(_output.WriteLine);
        Assert.True(nativeDir is not null, NativeDirMissing);

        var workerPath = Path.Combine(nativeDir!, WorkerBinaryName());

        // Ensure the P/Invoke resolver can load libpagespeed from nativeDir
        // even when the test-output runtimes/<rid>/native/ layout is absent.
        InstallNativeResolver(nativeDir, _output);

        // The JPEG fixture served at /test-image.jpg.
        var jpegBytes = LoadJpegFixture();
        Assert.True(jpegBytes.Length > 10_000,
            $"fixture too small ({jpegBytes.Length} bytes) to demonstrate savings");

        var cacheDir = Path.Combine(
            Path.GetTempPath(), $"ps_imgvariant_{Guid.NewGuid():N}");
        Directory.CreateDirectory(cacheDir);
        var cacheVolume = Path.Combine(cacheDir, "volume.dat");
        var socketPath = OperatingSystem.IsWindows()
            ? $"ps_imgvariant_{Guid.NewGuid():N}"          // pipe name (Win)
            : $"/tmp/ps_imgvariant_{Guid.NewGuid():N}.sock"; // UDS (Unix)

        Process? worker = null;
        IHost? host = null;
        try
        {
            // ── Spawn the real worker, wired to the same cache volume +
            //    socket the middleware will use. ─────────────────────────
            var apiPort = AllocateLoopbackPort();
            _output.WriteLine(
                $"launching worker {workerPath} api-port={apiPort} cache={cacheVolume}");
            worker = SpawnWorker(workerPath, cacheVolume, socketPath, apiPort);

            using (var workerClient = NewClient(apiPort))
            {
                await WaitForWorkerReadyAsync(workerClient, _output);
            }

            // ── Boot a TestServer with the real middleware + real native
            //    cache, pointed at the SAME cache volume + socket. ───────
            host = await BuildHostAsync(cacheVolume, socketPath, jpegBytes);
            var client = host.GetTestClient();

            // (3) Warm the cache with JPEG-only Accept: passthrough.
            byte[]? lastJpegBody = null;
            for (int i = 0; i < 10; i++)
            {
                using var resp = await GetImageAsync(client, "image/jpeg,image/png");
                Assert.Equal(HttpStatusCode.OK, resp.StatusCode);
                lastJpegBody = await resp.Content.ReadAsByteArrayAsync();
            }
            Assert.NotNull(lastJpegBody);
            Assert.Equal(jpegBytes.Length, lastJpegBody!.Length);
            _output.WriteLine(
                $"JPEG-Accept body length = {lastJpegBody.Length} (fixture = {jpegBytes.Length})");

            // (4)+(5) Drive WebP/AVIF Accept and bounded-poll for the
            // variant to materialize. The worker transcodes asynchronously
            // after notification, so distinguish "slow to warm" from
            // "never optimizes" with a retry loop rather than concluding
            // breakage on the first all-passthrough read. The 180 s bound
            // covers native assets built without optimization, where the
            // WebP/AVIF quality search takes over a minute; an optimized
            // build produces the variant within seconds.
            HttpResponseMessage? variantResp = null;
            byte[] variantBody = Array.Empty<byte>();
            string? variantCt = null;
            var pollTimer = Stopwatch.StartNew();
            var pollDeadline = TimeSpan.FromSeconds(180);
            for (int i = 0; pollTimer.Elapsed < pollDeadline; i++)
            {
                variantResp?.Dispose();
                variantResp = await GetImageAsync(
                    client, "image/webp,image/avif,*/*;q=0.8");
                Assert.Equal(HttpStatusCode.OK, variantResp.StatusCode);
                variantBody = await variantResp.Content.ReadAsByteArrayAsync();
                variantCt = variantResp.Content.Headers.ContentType?.MediaType;

                if (variantBody.Length < jpegBytes.Length &&
                    (variantCt == "image/webp" || variantCt == "image/avif"))
                {
                    _output.WriteLine(
                        $"variant ready after poll #{i + 1} ({pollTimer.Elapsed.TotalSeconds:F1}s): "
                        + $"{variantBody.Length} bytes, ct={variantCt}");
                    break;
                }

                await Task.Delay(500);
            }

            Assert.NotNull(variantResp);

            // Diagnostic: dump worker stats so a failure shows whether the
            // worker received notifications and wrote image variants.
            using (var sc = NewClient(apiPort))
            {
                try
                {
                    var stats = await sc.GetStringAsync("/v1/stats");
                    _output.WriteLine($"[worker /v1/stats] {stats}");
                }
                catch (Exception ex) { _output.WriteLine($"stats fetch failed: {ex.Message}"); }
            }

            // (5) The WebP/AVIF response must be MATERIALLY smaller AND a
            // transcoded image format.
            Assert.True(
                variantBody.Length < jpegBytes.Length,
                $"expected WebP/AVIF body ({variantBody.Length}) materially "
                + $"smaller than JPEG fixture ({jpegBytes.Length}); content "
                + "negotiation did not serve an optimized variant. "
                + "ct=" + (variantCt ?? "<none>"));
            Assert.True(
                variantCt == "image/webp" || variantCt == "image/avif",
                $"expected Content-Type image/webp or image/avif, got '{variantCt}'");

            // The variant should be the bulk of the savings, not a rounding
            // error — require at least a 2x reduction to guard against a
            // near-identity "optimization".
            Assert.True(
                variantBody.Length * 2 < jpegBytes.Length,
                $"variant ({variantBody.Length}) not materially smaller than "
                + $"half the JPEG fixture ({jpegBytes.Length})");

            // (6) Vary must contain Accept on BOTH the passthrough and the
            // variant responses (content negotiation correctness contract).
            AssertVaryContainsAccept(variantResp!, _output, "webp/avif");

            using (var jpegResp = await GetImageAsync(client, "image/jpeg,image/png"))
                AssertVaryContainsAccept(jpegResp, _output, "jpeg");
        }
        finally
        {
            if (host is not null)
            {
                try { await host.StopAsync(TimeSpan.FromSeconds(5)); } catch { }
                host.Dispose();
            }
            if (worker is { HasExited: false })
            {
                try
                {
                    worker.Kill(entireProcessTree: true);
                    worker.WaitForExit(5000);
                }
                catch { }
            }
            worker?.Dispose();
            try { Directory.Delete(cacheDir, recursive: true); } catch { }
            if (!OperatingSystem.IsWindows())
            {
                try { File.Delete(socketPath); } catch { }
            }
        }
    }

    // -------------------------------------------------------------------
    // Host / middleware harness
    // -------------------------------------------------------------------

    private static async Task<IHost> BuildHostAsync(
        string cacheVolume, string socketPath, byte[] jpegBytes)
    {
        var hostBuilder = new HostBuilder()
            .ConfigureWebHost(webHost =>
            {
                webHost
                    .UseTestServer()
                    .ConfigureServices(services =>
                    {
                        services.AddPageSpeed(o =>
                        {
                            o.Enabled = true;
                            o.Cache.VolumePath = cacheVolume;
                            o.Cache.VolumeSizeBytes = 64L * 1024 * 1024;
                            // A worker run as a separate process: we spawn it
                            // ourselves, so WorkerProcessHost must not
                            // (AutoStart=false), and the middleware notifies
                            // it at the configured SocketPath.
                            o.Worker.AutoStart = false;
                            o.Worker.SocketPath = socketPath;
                            o.Worker.ApiPort = 0;
                        });
                    })
                    .Configure(app =>
                    {
                        // Production sample ordering: PageSpeed BEFORE the
                        // terminal asset handler. PageSpeed wraps the response
                        // body, so the asset bytes flow back up through it.
                        app.UsePageSpeed();

                        // Terminal: serve the JPEG fixture at /test-image.jpg.
                        app.Run(async ctx =>
                        {
                            if (ctx.Request.Path == "/test-image.jpg")
                            {
                                ctx.Response.ContentType = "image/jpeg";
                                ctx.Response.Headers.CacheControl =
                                    "public, max-age=600";
                                await ctx.Response.Body.WriteAsync(jpegBytes);
                                return;
                            }
                            ctx.Response.StatusCode = 404;
                        });
                    });
            });

        var host = await hostBuilder.StartAsync();
        return host;
    }

    private static Task<HttpResponseMessage> GetImageAsync(
        HttpClient client, string acceptHeader)
    {
        var req = new HttpRequestMessage(HttpMethod.Get, "/test-image.jpg");
        req.Headers.Accept.Clear();
        foreach (var part in acceptHeader.Split(','))
            req.Headers.Accept.ParseAdd(part.Trim());
        return client.SendAsync(req);
    }

    private static void AssertVaryContainsAccept(
        HttpResponseMessage resp, ITestOutputHelper output, string label)
    {
        var vary = resp.Headers.TryGetValues("Vary", out var v)
            ? string.Join(", ", v) : "";
        output.WriteLine($"[{label}] Vary: {vary}");
        Assert.Contains("Accept", vary, StringComparison.OrdinalIgnoreCase);
    }

    // -------------------------------------------------------------------
    // Worker subprocess (spawned directly; the test owns the process tree)
    // -------------------------------------------------------------------

    private Process SpawnWorker(
        string workerPath, string cacheVolume, string socketPath, int apiPort)
    {
        var psi = new ProcessStartInfo
        {
            FileName = workerPath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        psi.ArgumentList.Add("--cache-path"); psi.ArgumentList.Add(cacheVolume);
        psi.ArgumentList.Add("--cache-size"); psi.ArgumentList.Add((64L * 1024 * 1024).ToString());
        psi.ArgumentList.Add("--socket");     psi.ArgumentList.Add(socketPath);
        psi.ArgumentList.Add("--api-port");   psi.ArgumentList.Add(apiPort.ToString());
        // Loopback + no token needs the explicit opt-out,
        // matching what WorkerProcessHost passes in production.
        psi.ArgumentList.Add("--api-no-auth");

        var p = Process.Start(psi)
            ?? throw new InvalidOperationException("Process.Start returned null");

        _ = Task.Run(async () =>
        {
            try
            {
                while (await p.StandardOutput.ReadLineAsync() is { } line)
                    _output.WriteLine($"[worker stdout] {line}");
            }
            catch { }
        });
        _ = Task.Run(async () =>
        {
            try
            {
                while (await p.StandardError.ReadLineAsync() is { } line)
                    _output.WriteLine($"[worker stderr] {line}");
            }
            catch { }
        });

        return p;
    }

    private static HttpClient NewClient(int apiPort) => new()
    {
        BaseAddress = new Uri($"http://127.0.0.1:{apiPort}"),
        Timeout = TimeSpan.FromSeconds(10),
    };

    private static async Task WaitForWorkerReadyAsync(
        HttpClient client, ITestOutputHelper output)
    {
        for (int i = 0; i < 60; i++)
        {
            try
            {
                using var r = await client.GetAsync("/v1/health");
                if (r.IsSuccessStatusCode) return;
            }
            catch (HttpRequestException) { }
            catch (TaskCanceledException) { }
            await Task.Delay(500);
        }
        throw new TimeoutException(
            $"Worker /v1/health never reached 200 within ~30s on {client.BaseAddress}");
    }

    private static int AllocateLoopbackPort()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        try { return ((IPEndPoint)l.LocalEndpoint).Port; }
        finally { l.Stop(); }
    }

    // -------------------------------------------------------------------
    // Native asset resolution
    // -------------------------------------------------------------------

    private static string WorkerBinaryName() =>
        OperatingSystem.IsWindows() ? "factory_worker.exe" : "factory_worker";

    internal static string NativeLibName() =>
        OperatingSystem.IsWindows() ? "pagespeed.dll"
        : OperatingSystem.IsMacOS() ? "libpagespeed.dylib"
        : "libpagespeed.so";

    private const string NativeDirMissing =
        "native dir not found. Set PAGESPEED_E2E_NATIVE_DIR to a directory "
        + "holding factory_worker + libpagespeed, build with "
        + "PageSpeedNativeAssetsDir, or restore a "
        + "WeAmp.PageSpeed.NativeAssets.* package.";

    /// <summary>
    /// Skipped (never reported as passed) when <see cref="ResolveNativeDir"/>
    /// finds no worker, unless <c>PAGESPEED_E2E_REQUIRE_NATIVE=1</c> requires
    /// the native assets, in which case the test runs and fails.
    /// </summary>
    public sealed class ImageVariantFactAttribute : FactAttribute
    {
        public ImageVariantFactAttribute()
        {
            if (NativeAssetsRequirement.Required) return;
            if (ResolveNativeDir(null) is null)
                Skip = NativeAssetsRequirement.SkipReason(NativeDirMissing);
        }
    }

    /// <summary>
    /// Resolves a directory holding both factory_worker and libpagespeed:
    ///   1. PAGESPEED_E2E_NATIVE_DIR (explicit override)
    ///   2. test-output runtimes/&lt;rid&gt;/native/ (NuGet runtime layout)
    ///   3. the restored NuGet cache for the NativeAssets package version
    ///      matching the loaded WeAmp.PageSpeed.AspNetCore assembly.
    /// Returns null when none contain a worker binary.
    /// </summary>
    internal static string? ResolveNativeDir(Action<string>? log)
    {
        var candidates = new List<string>();

        var explicitDir = Environment.GetEnvironmentVariable("PAGESPEED_E2E_NATIVE_DIR");
        if (!string.IsNullOrWhiteSpace(explicitDir))
            candidates.Add(explicitDir);

        var rid = RuntimeInformation.RuntimeIdentifier;
        candidates.Add(Path.Combine(
            AppContext.BaseDirectory, "runtimes", rid, "native"));

        // NuGet global-packages cache fallback. The native binaries ship in
        // WeAmp.PageSpeed.NativeAssets.{macOS,Linux,Windows}; resolve the
        // version from the AspNetCore assembly's informational version.
        var nugetRoot = NuGetGlobalPackagesRoot();
        if (nugetRoot is not null)
        {
            var pkg = OperatingSystem.IsWindows()
                ? "weamp.pagespeed.nativeassets.windows"
                : OperatingSystem.IsMacOS()
                    ? "weamp.pagespeed.nativeassets.macos"
                    : "weamp.pagespeed.nativeassets.linux";
            var pkgDir = Path.Combine(nugetRoot, pkg);
            if (Directory.Exists(pkgDir))
            {
                // Prefer the highest installed version.
                foreach (var verDir in Directory.GetDirectories(pkgDir)
                             .OrderByDescending(d => d))
                {
                    candidates.Add(Path.Combine(
                        verDir, "runtimes", rid, "native"));
                }
            }
        }

        foreach (var dir in candidates)
        {
            if (dir is null) continue;
            if (File.Exists(Path.Combine(dir, WorkerBinaryName())))
            {
                log?.Invoke($"resolved native dir: {dir}");
                return dir;
            }
        }
        return null;
    }

    private static string? NuGetGlobalPackagesRoot()
    {
        var env = Environment.GetEnvironmentVariable("NUGET_PACKAGES");
        if (!string.IsNullOrWhiteSpace(env) && Directory.Exists(env))
            return env;
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var def = Path.Combine(home, ".nuget", "packages");
        return Directory.Exists(def) ? def : null;
    }

    /// <summary>
    /// Forces the PageSpeed P/Invoke resolver to load libpagespeed from the
    /// resolved native dir. The library's own [ModuleInitializer] resolver
    /// only probes AppContext.BaseDirectory/runtimes/&lt;rid&gt;/native, which
    /// is empty in the test output, so we register a higher-priority
    /// resolver pointing at the same dir as the worker.
    /// </summary>
    private static void InstallNativeResolver(string nativeDir, ITestOutputHelper output)
    {
        var libPath = Path.Combine(nativeDir, NativeLibName());
        if (!File.Exists(libPath))
        {
            output.WriteLine($"WARN: native lib not at {libPath}; relying on default resolver");
            return;
        }

        // Resolver is per-assembly; the [DllImport] lives in WeAmp.PageSpeed.
        var nativeAsm = typeof(WeAmp.PageSpeed.PageSpeedCache).Assembly;
        try
        {
            NativeLibrary.SetDllImportResolver(nativeAsm, (name, asm, path) =>
            {
                if (name == "pagespeed" &&
                    NativeLibrary.TryLoad(libPath, out var h))
                    return h;
                return IntPtr.Zero;
            });
            output.WriteLine($"installed native resolver -> {libPath}");
        }
        catch (InvalidOperationException)
        {
            // A resolver was already set for this assembly (e.g. the
            // [ModuleInitializer] ran). Pre-load the lib so the default
            // resolver's later TryLoad of the same soname hits the cache.
            NativeLibrary.TryLoad(libPath, out _);
            output.WriteLine($"pre-loaded native lib (resolver already set) -> {libPath}");
        }
    }

    private static byte[] LoadJpegFixture()
    {
        // Prefer an env-supplied fixture; otherwise use the fixture committed
        // alongside the test (copied to output via the csproj), then fall back
        // to a sample wwwroot image relative to the source tree.
        var envFixture = Environment.GetEnvironmentVariable("PAGESPEED_E2E_JPEG_FIXTURE");
        if (!string.IsNullOrWhiteSpace(envFixture) && File.Exists(envFixture))
            return File.ReadAllBytes(envFixture);

        var bundled = Path.Combine(
            AppContext.BaseDirectory, "TestFixtures", "test-image.jpg");
        if (File.Exists(bundled)) return File.ReadAllBytes(bundled);

        // samples/aspnetcore/test/<proj>/bin/<cfg>/<tfm>/  →  up to aspnetcore
        var probe = AppContext.BaseDirectory;
        for (int i = 0; i < 8 && probe is not null; i++)
        {
            foreach (var rel in new[]
            {
                Path.Combine("samples", "DemoSite", "wwwroot", "img", "hero.jpg"),
                Path.Combine("samples", "DemoSite", "wwwroot", "img", "photo-1.jpg"),
            })
            {
                var candidate = Path.Combine(probe, rel);
                if (File.Exists(candidate)) return File.ReadAllBytes(candidate);
            }
            probe = Directory.GetParent(probe)?.FullName;
        }

        throw new FileNotFoundException(
            "JPEG fixture not found. Set PAGESPEED_E2E_JPEG_FIXTURE to a .jpg, "
            + "or ensure TestFixtures/test-image.jpg ships to the test output.");
    }
}
