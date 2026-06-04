using System.Net;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Options;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Pins Kestrel to the internal sidecar RAW-ORIGIN so the spawned nginx and
/// Kestrel agree on the SAME upstream. This is the RAW-ORIGIN configurator;
/// the public-bind for Inverse lives in a SEPARATE
/// <see cref="PublicKestrelConfigureOptions"/> so a bug in one cannot widen the
/// other (review fix). Active when PageSpeed is Enabled AND Sidecar.Mode is
/// Process or Inverse.
///
/// <para><b>Process</b> (front-proxy): nginx is the public front-end, Kestrel is
/// the origin. Resolve the origin transport ONCE and bind it:
/// <list type="bullet">
///   <item>OriginPort == 0 (default): prefer a Unix-domain socket in a short,
///   0700 per-app directory (lowest attack surface, no port collisions); fall
///   back to an ephemeral loopback port if a usable UDS path can't be built.</item>
///   <item>OriginPort &gt; 0: honor that exact loopback port.</item>
/// </list></para>
///
/// <para><b>Inverse</b>: Kestrel owns the public socket (bound elsewhere); the
/// bundled nginx runs loopback-only as an optimize-proxy and proxy_pass-es back
/// to a SECOND, PRIVATE raw-origin Kestrel endpoint configured here. The raw
/// origin is ALWAYS a forced loopback-TCP port (NEVER a UDS) so
/// <c>context.Connection.LocalPort</c> is the authoritative, kernel-set
/// loop-break port; every accepted raw-origin connection is tagged with an
/// <see cref="IRawOriginMarker"/> connection feature (set by the listener, never
/// by the peer) as the PRIMARY loop-break signal. The private nginx loopback
/// listen port is also allocated and published here.</para>
///
/// Loopback only — never <see cref="IPAddress.Any"/>. For Docker / External
/// modes this is a no-op: nginx is not co-located, so the host keeps whatever
/// Kestrel endpoints it configured itself.
/// </summary>
internal sealed class SidecarKestrelConfigureOptions : IConfigureOptions<KestrelServerOptions>
{
    private readonly IOptions<PageSpeedOptions> _options;
    private readonly InternalSidecarEndpoint _endpoint;
    private readonly ILogger<SidecarKestrelConfigureOptions> _logger;

    public SidecarKestrelConfigureOptions(
        IOptions<PageSpeedOptions> options,
        InternalSidecarEndpoint endpoint,
        ILogger<SidecarKestrelConfigureOptions> logger)
    {
        _options = options;
        _endpoint = endpoint;
        _logger = logger;
    }

    public void Configure(KestrelServerOptions kestrel)
    {
        var ps = _options.Value;
        if (!ps.Enabled)
        {
            return; // PageSpeed off: do not touch the host's Kestrel endpoints.
        }

        if (ps.Sidecar.Mode == SidecarMode.Inverse)
        {
            ConfigureInverseRawOrigin(kestrel, ps);
            return;
        }

        if (ps.Sidecar.Mode != SidecarMode.Process)
        {
            // nginx not co-located (Docker/External): do not touch endpoints.
            return;
        }

        // ---- Process (front-proxy): nginx is the public front-end -----------
        // Resolve the transport ONCE. SetPort/SetSocketPath are idempotent on
        // first set and throw on conflicting reassignment, so a stray second
        // call can never desync Kestrel from the generated nginx config.
        var originPort = ps.Sidecar.OriginPort;
        if (originPort > 0)
        {
            // Operator pinned a loopback origin port — honor it exactly.
            _endpoint.SetPort(originPort);
            kestrel.Listen(IPAddress.Loopback, originPort);
            _logger.LogDebug("Sidecar origin pinned to configured loopback port {Port}", originPort);
            return;
        }

        // OriginPort == 0: prefer a Unix-domain socket; fall back to ephemeral loopback.
        var socketPath = ResolveSocketPath(ps.Sidecar);
        if (socketPath != null)
        {
            _endpoint.SetSocketPath(socketPath);
            kestrel.ListenUnixSocket(socketPath);
            _logger.LogDebug("Sidecar origin pinned to Unix-domain socket {Socket}", socketPath);
            return;
        }

        var ephemeral = EphemeralLoopbackPort.Allocate();
        _endpoint.SetPort(ephemeral);
        kestrel.Listen(IPAddress.Loopback, ephemeral);
        _logger.LogDebug("Sidecar origin pinned to auto-allocated ephemeral loopback port {Port}", ephemeral);
    }

    /// <summary>
    /// Inverse raw-origin bind. FORCES a loopback-TCP raw origin (never a UDS) so
    /// the loop break has an authoritative kernel-set LocalPort, tags every
    /// accepted connection with the <see cref="IRawOriginMarker"/> feature (the
    /// PRIMARY, listener-set loop-break signal), and allocates+publishes the
    /// private nginx loopback listen port. Binds the raw origin on
    /// <see cref="IPAddress.Loopback"/> ONLY — never <see cref="IPAddress.Any"/> —
    /// and never touches the public endpoint (that is
    /// <see cref="PublicKestrelConfigureOptions"/>'s job).
    /// </summary>
    private void ConfigureInverseRawOrigin(KestrelServerOptions kestrel, PageSpeedOptions ps)
    {
        // Raw origin: forced loopback-TCP. OriginPort if pinned, else ephemeral.
        var rawOriginPort = ps.Sidecar.OriginPort > 0
            ? ps.Sidecar.OriginPort
            : EphemeralLoopbackPort.Allocate();

        _endpoint.SetPort(rawOriginPort);
        _endpoint.SetRawOriginLoopbackPort(rawOriginPort);

        // Tag every accepted connection on THIS listener with the unforgeable
        // raw-origin marker (set by the listener, never by the peer). This is the
        // PRIMARY loop-break signal the middleware reads first.
        kestrel.Listen(IPAddress.Loopback, rawOriginPort, listenOptions =>
        {
            listenOptions.Use(next => context =>
            {
                context.Features.Set<IRawOriginMarker>(RawOriginMarker.Instance);
                return next(context);
            });
        });

        // Private nginx loopback listen port — decoupled from the raw-origin port.
        // When BOTH are auto-allocated the kernel can hand back the SAME number
        // (EphemeralLoopbackPort releases each probe before the next), and the
        // validator only catches EXPLICIT collisions — so re-probe until distinct
        // from the raw origin (bounded; throw if it cannot, which is fail-safe:
        // nginx then can't bind and the sidecar degrades rather than mis-routing).
        int nginxPort;
        if (ps.Sidecar.NginxLoopbackPort > 0)
        {
            nginxPort = ps.Sidecar.NginxLoopbackPort;
        }
        else
        {
            nginxPort = EphemeralLoopbackPort.Allocate();
            for (var i = 0; i < 5 && nginxPort == rawOriginPort; i++)
            {
                nginxPort = EphemeralLoopbackPort.Allocate();
            }
        }
        if (nginxPort == rawOriginPort)
        {
            throw new InvalidOperationException(
                "PageSpeed Inverse: could not allocate a nginx loopback port distinct from the " +
                $"raw-origin port ({rawOriginPort}); set Sidecar.NginxLoopbackPort/OriginPort explicitly.");
        }
        _endpoint.SetNginxLoopbackPort(nginxPort);

        // Mint the defense-in-depth nonce (the transformer injects it; the
        // middleware verifies+strips it but never treats it as a sole gate).
        _ = _endpoint.InternalNonce;

        _logger.LogDebug(
            "Inverse: raw-origin loopback-TCP port {RawOrigin}, nginx loopback port {Nginx}",
            rawOriginPort, nginxPort);
    }

    /// <summary>
    /// Resolves the origin socket path honoring <see cref="SidecarOptions.SocketPath"/>'s
    /// 3-state contract: an explicit non-empty path is used verbatim; an empty
    /// string forces loopback TCP (returns null); null auto-builds a private path.
    /// </summary>
    private string? ResolveSocketPath(SidecarOptions sidecar)
    {
        if (sidecar.SocketPath != null)
        {
            return sidecar.SocketPath.Length == 0 ? null : sidecar.SocketPath;
        }
        return TryBuildUnixSocketPath();
    }

    /// <summary>
    /// Builds a short, per-app Unix-domain socket path under a 0700 directory,
    /// or returns null if a usable path can't be created (caller falls back to
    /// loopback). Kept short to stay under the sun_path limit (108 bytes on
    /// Linux, 104 on macOS) — see the design record P1 sun_path caveat.
    /// </summary>
    private string? TryBuildUnixSocketPath()
    {
        if (!OperatingSystem.IsLinux() && !OperatingSystem.IsMacOS())
        {
            return null; // Windows: nginx UDS upstream not used; loopback only.
        }

        try
        {
            // Short base: /tmp on macOS (Path.GetTempPath() there is the long
            // /var/folders/.../T/ path), $TMPDIR-or-/tmp on Linux.
            var baseDir = OperatingSystem.IsMacOS()
                ? "/tmp"
                : (Environment.GetEnvironmentVariable("TMPDIR") is { Length: > 0 } t ? t : "/tmp");

            // Short per-app dir name (ps-<pid>-<8hex>) so multiple AddPageSpeed
            // hosts in one process and successive restarts never collide.
            var token = Guid.NewGuid().ToString("N").Substring(0, 8);
            var appDir = Path.Combine(baseDir, $"ps-{Environment.ProcessId}-{token}");
            Directory.CreateDirectory(appDir);
            // 0700: only this user may traverse into the dir and connect to the socket.
            if (OperatingSystem.IsLinux() || OperatingSystem.IsMacOS())
            {
                File.SetUnixFileMode(appDir,
                    UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            }

            var sock = Path.Combine(appDir, "origin.sock");

            // sun_path guard: bail to loopback rather than truncate-and-fail.
            var max = OperatingSystem.IsMacOS() ? 104 : 108;
            if (System.Text.Encoding.UTF8.GetByteCount(sock) >= max)
            {
                _logger.LogWarning(
                    "Computed UDS path '{Path}' exceeds sun_path limit ({Max}); falling back to loopback",
                    sock, max);
                return null;
            }

            // A stale socket from a crashed prior run would make bind() fail.
            if (File.Exists(sock))
            {
                File.Delete(sock);
            }

            return sock;
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed to prepare a Unix-domain socket origin; falling back to loopback");
            return null;
        }
    }
}
