using System.Globalization;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Singleton holding the rendezvous coordinates for the nginx sidecar's
/// reverse-proxy origin: the loopback port Kestrel listens on, or the
/// Unix-domain socket path Kestrel binds. The topology is INVERTED relative
/// to the 2.0 worker model — here the spawned nginx is the front-end reverse
/// proxy and Kestrel is the upstream origin, so all three parties must agree
/// on the SAME origin coordinate:
/// <list type="bullet">
///   <item>the <c>IConfigureOptions&lt;KestrelServerOptions&gt;</c> that binds
///   Kestrel (it resolves the endpoint and pins it here),</item>
///   <item>the nginx config generator, which reads
///   <see cref="ProxyPassTarget"/> to emit the <c>proxy_pass</c> upstream,</item>
///   <item>the spawned nginx process, which dials that upstream.</item>
/// </list>
/// Written once by the Kestrel configurator during host build; read by the
/// nginx config generator.
/// </summary>
/// <remarks>
/// <para>
/// Exactly one of <see cref="Port"/> / <see cref="SocketPath"/> is meaningful:
/// when a socket path is set, <see cref="UseUnixSocket"/> is <c>true</c> and the
/// port is ignored; otherwise the loopback port is used.
/// </para>
/// <para>
/// Thread-safe: a single internal lock guards both fields and both are
/// idempotent on first set — a second set with the same value is a no-op, and
/// a second set with a different value throws so an accidental double-publish
/// cannot silently desynchronize Kestrel from the generated nginx config.
/// Once one transport is chosen (port or socket), the other cannot be set.
/// </para>
/// </remarks>
public sealed class InternalSidecarEndpoint
{
    private readonly object _gate = new();
    private int _port;
    private string? _socketPath;
    // Inverse-mode rendezvous (set once by the raw-origin Kestrel configurator).
    private int _rawOriginLoopbackPort;
    private int _nginxLoopbackPort;
    private byte[]? _internalNonce;

    // The socket path is emitted verbatim into the generated `proxy_pass
    // http://unix:<path>:` AND bound by Kestrel. Sidecar.SocketPath is operator-
    // tainted config, so restrict it fail-closed to an absolute path of safe chars
    // (no ; { } # whitespace " ' backslash $) — otherwise it is the one config-
    // injection vector that bypasses the generator's RejectInjection.
    private static readonly Regex SafeSocketPath = new(@"^/[A-Za-z0-9._/-]+$", RegexOptions.Compiled);

    /// <summary>
    /// Loopback port Kestrel listens on, or 0 if a Unix-domain socket is used
    /// or no transport has been chosen yet.
    /// </summary>
    public int Port
    {
        get { lock (_gate) { return _port; } }
    }

    /// <summary>
    /// Unix-domain socket path Kestrel binds, or <c>null</c> when a loopback
    /// port is used (or no transport has been chosen yet).
    /// </summary>
    public string? SocketPath
    {
        get { lock (_gate) { return _socketPath; } }
    }

    /// <summary>
    /// <c>true</c> when the origin is a Unix-domain socket; <c>false</c> when it
    /// is a loopback TCP port (or nothing has been chosen yet).
    /// </summary>
    public bool UseUnixSocket
    {
        get { lock (_gate) { return _socketPath != null; } }
    }

    /// <summary>
    /// Sets the loopback port the Kestrel origin listens on. Idempotent: a
    /// second call with the same port is a no-op; a second call with a
    /// different port — or any call after a socket path was chosen — throws.
    /// </summary>
    public void SetPort(int port)
    {
        if (port <= 0 || port > 65535)
            throw new ArgumentOutOfRangeException(nameof(port), port,
                "port must be in [1, 65535]");

        lock (_gate)
        {
            if (_socketPath != null)
                throw new InvalidOperationException(
                    $"InternalSidecarEndpoint already bound to Unix socket '{_socketPath}'; cannot set loopback port {port}");

            if (_port == 0)
            {
                _port = port;
            }
            else if (_port != port)
            {
                throw new InvalidOperationException(
                    $"InternalSidecarEndpoint already set to port {_port}; cannot reassign to {port}");
            }
        }
    }

    /// <summary>
    /// Sets the Unix-domain socket path the Kestrel origin binds. Idempotent: a
    /// second call with the same path is a no-op; a second call with a different
    /// path — or any call after a loopback port was chosen — throws.
    /// </summary>
    public void SetSocketPath(string socketPath)
    {
        if (string.IsNullOrWhiteSpace(socketPath))
            throw new ArgumentException(
                "socket path must be a non-empty path", nameof(socketPath));
        if (!SafeSocketPath.IsMatch(socketPath))
            throw new ArgumentException(
                "socket path must be an absolute path of [A-Za-z0-9._/-] — it is emitted into the nginx proxy_pass upstream and must not contain config metacharacters (fail-closed)",
                nameof(socketPath));

        lock (_gate)
        {
            if (_port != 0)
                throw new InvalidOperationException(
                    $"InternalSidecarEndpoint already set to port {_port}; cannot set Unix socket '{socketPath}'");

            if (_socketPath == null)
            {
                _socketPath = socketPath;
            }
            else if (!string.Equals(_socketPath, socketPath, StringComparison.Ordinal))
            {
                throw new InvalidOperationException(
                    $"InternalSidecarEndpoint already bound to Unix socket '{_socketPath}'; cannot reassign to '{socketPath}'");
            }
        }
    }

    /// <summary>
    /// Returns the nginx <c>proxy_pass</c> upstream target for the chosen origin:
    /// <list type="bullet">
    ///   <item><c>http://unix:/path/to.sock:</c> for a Unix-domain socket
    ///   (note nginx's trailing colon, which separates the socket path from the
    ///   optional URI suffix),</item>
    ///   <item><c>http://127.0.0.1:&lt;port&gt;</c> for a loopback TCP port.</item>
    /// </list>
    /// Throws if no transport has been chosen yet — the generator must run after
    /// Kestrel has been configured.
    /// </summary>
    public string ProxyPassTarget()
    {
        lock (_gate)
        {
            if (_socketPath != null)
                return $"http://unix:{_socketPath}:";
            if (_port != 0)
                return string.Create(CultureInfo.InvariantCulture, $"http://127.0.0.1:{_port}");
            throw new InvalidOperationException(
                "InternalSidecarEndpoint has no transport set; Kestrel must be configured before the nginx config is generated");
        }
    }

    /// <summary>
    /// Kestrel-side helper: the loopback origin port to bind, or <c>null</c> when
    /// a Unix-domain socket is in use. Convenience for the Kestrel configurator
    /// after it has pinned the endpoint, so the bind site and the rendezvous
    /// stay in lockstep.
    /// </summary>
    public int? KestrelListenPort
    {
        get
        {
            lock (_gate)
            {
                return _socketPath != null ? null : (_port != 0 ? _port : null);
            }
        }
    }

    // ----------------------------------------------------------------------
    // Inverse-mode coordinates. Three distinct sockets live
    // in one process: the public Kestrel port (Sidecar.ListenPort, owned by the
    // host/PublicKestrelConfigureOptions), the private nginx loopback listen
    // port (NginxLoopbackPort), and the private raw-origin loopback-TCP Kestrel
    // port (RawOriginLoopbackPort, == Port in Inverse). The loop break is keyed
    // on a per-connection RawOrigin marker feature (set by the raw-origin Kestrel
    // listener — see RawOriginConnectionMarker) AND, redundantly, on
    // RawOriginLoopbackPort; either alone is a kernel/listener-set, unforgeable
    // signal. The InternalNonce is defense-in-depth only, NEVER a sole gate.
    // ----------------------------------------------------------------------

    /// <summary>
    /// The authoritative loop-break port: the private raw-origin loopback-TCP
    /// Kestrel port nginx proxy_pass-es back to in Inverse mode. Equal to
    /// <see cref="Port"/> in Inverse, but exposed explicitly so the middleware's
    /// loop break never depends on UDS-vs-TCP ambiguity. 0 when not set
    /// (Process/External, or before the raw-origin configurator runs).
    /// </summary>
    public int RawOriginLoopbackPort
    {
        get { lock (_gate) { return _rawOriginLoopbackPort; } }
    }

    /// <summary>
    /// The loopback-only TCP port the bundled nginx listens on in Inverse mode
    /// (decoupled from the raw-origin port). 0 when not set.
    /// </summary>
    public int NginxLoopbackPort
    {
        get { lock (_gate) { return _nginxLoopbackPort; } }
    }

    /// <summary>
    /// The per-process internal nonce, minted once via
    /// <see cref="RandomNumberGenerator.GetBytes(int)"/>. Defense-in-depth token
    /// the forward transformer injects as <c>X-PageSpeed-Internal</c>; the
    /// middleware verifies+strips it but NEVER treats it as a sole gate (the
    /// kernel/listener-set marker + RawOriginLoopbackPort are authoritative).
    /// Minted lazily on first read so it exists even in Process/External tests.
    /// </summary>
    public byte[] InternalNonce
    {
        get
        {
            lock (_gate)
            {
                _internalNonce ??= RandomNumberGenerator.GetBytes(16);
                return _internalNonce;
            }
        }
    }

    /// <summary>
    /// Sets the authoritative raw-origin loop-break port. Idempotent: a second
    /// call with the same value is a no-op; a different value throws.
    /// </summary>
    public void SetRawOriginLoopbackPort(int port)
    {
        if (port <= 0 || port > 65535)
            throw new ArgumentOutOfRangeException(nameof(port), port, "port must be in [1, 65535]");
        lock (_gate)
        {
            if (_rawOriginLoopbackPort == 0)
                _rawOriginLoopbackPort = port;
            else if (_rawOriginLoopbackPort != port)
                throw new InvalidOperationException(
                    $"InternalSidecarEndpoint raw-origin port already set to {_rawOriginLoopbackPort}; cannot reassign to {port}");
        }
    }

    /// <summary>
    /// Sets the private nginx loopback listen port. Idempotent: a second call
    /// with the same value is a no-op; a different value throws.
    /// </summary>
    public void SetNginxLoopbackPort(int port)
    {
        if (port <= 0 || port > 65535)
            throw new ArgumentOutOfRangeException(nameof(port), port, "port must be in [1, 65535]");
        lock (_gate)
        {
            if (_nginxLoopbackPort == 0)
                _nginxLoopbackPort = port;
            else if (_nginxLoopbackPort != port)
                throw new InvalidOperationException(
                    $"InternalSidecarEndpoint nginx loopback port already set to {_nginxLoopbackPort}; cannot reassign to {port}");
        }
    }

    /// <summary>
    /// True when <paramref name="localPort"/> is the authoritative raw-origin
    /// loop-break port (and that port has been set). The redundant kernel-set
    /// second signal behind the per-connection RawOrigin marker feature.
    /// </summary>
    public bool IsRawOriginLocalPort(int localPort)
    {
        lock (_gate)
        {
            return _rawOriginLoopbackPort != 0 && _rawOriginLoopbackPort == localPort;
        }
    }
}
