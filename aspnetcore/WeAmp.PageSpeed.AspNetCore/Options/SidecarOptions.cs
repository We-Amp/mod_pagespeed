// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Sidecar mode for the PageSpeed nginx reverse-proxy process.
/// </summary>
public enum SidecarMode
{
    /// <summary>
    /// Spawn the bundled nginx as the PUBLIC front-end reverse proxy and run
    /// Kestrel as the private origin behind it (the shipped front-proxy model).
    /// </summary>
    Process,

    /// <summary>
    /// INVERSE topology (default): the ASP.NET Core middleware is the PUBLIC
    /// front door (Kestrel owns the public socket/TLS/auth/routing); the bundled
    /// nginx runs LOOPBACK-ONLY behind it as an optimize-proxy. The middleware
    /// streams optimizable requests over loopback to nginx, which proxy_pass-es
    /// back to a SECOND, PRIVATE raw-origin Kestrel endpoint where the middleware
    /// bypasses itself. Linux-only.
    /// </summary>
    Inverse,

    /// <summary>
    /// Manage nginx via a Docker container. Reserved for a future release and not
    /// implemented — Process and External cover the deployments this package targets.
    /// </summary>
    Docker,

    /// <summary>
    /// Connect to an externally-managed nginx (operator-run reverse proxy).
    /// </summary>
    External
}

/// <summary>
/// Configuration for the PageSpeed nginx sidecar process.
/// </summary>
public class SidecarOptions
{
    /// <summary>
    /// Sidecar mode: Inverse (default), Process, Docker, or External. This
    /// package defaults to <see cref="SidecarMode.Inverse"/> — Kestrel is
    /// the public front door, nginx runs loopback-only behind it (acceptable as
    /// the default only because the loop-break + admin-authz blockers are closed
    /// by construction).
    /// </summary>
    public SidecarMode Mode { get; set; } = SidecarMode.Inverse;

    /// <summary>
    /// The public port. In <see cref="SidecarMode.Process"/> this is the public
    /// TCP port the bundled nginx listens on. In <see cref="SidecarMode.Inverse"/>
    /// this is the public Kestrel port, auto-bound by the configurator ONLY when
    /// the operator configured no Kestrel endpoint (the common edge-TLS case
    /// leaves the operator bind untouched). Default: 8080.
    /// </summary>
    public int ListenPort { get; set; } = 8080;

    /// <summary>
    /// The raw-origin port. In <see cref="SidecarMode.Process"/>/External this is
    /// the loopback port the Kestrel origin listens on (0 = auto: prefer a
    /// Unix-domain socket in a private 0700 directory, fall back to an ephemeral
    /// loopback port). In <see cref="SidecarMode.Inverse"/> the raw origin is
    /// ALWAYS a forced loopback-TCP port (NEVER a UDS) so
    /// <c>context.Connection.LocalPort</c> is authoritative for the loop break:
    /// <see cref="OriginPort"/> if &gt; 0, else an auto ephemeral loopback port.
    /// Always loopback-only; never bound to a routable interface.
    /// </summary>
    public int OriginPort { get; set; } = 0;

    /// <summary>
    /// Origin Unix-domain socket path (3-state): <c>null</c> = auto (build one in
    /// a private 0700 directory when <see cref="OriginPort"/> is 0); empty string
    /// = force loopback TCP; an explicit path = bind that socket. Ignored when
    /// <see cref="OriginPort"/> &gt; 0. NOTE: a non-null UDS path is REJECTED by the
    /// validator in <see cref="SidecarMode.Inverse"/> (the loop break needs an
    /// authoritative loopback-TCP LocalPort); the UDS preference applies to
    /// Process/External only.
    /// </summary>
    public string? SocketPath { get; set; }

    /// <summary>
    /// Inverse only: the loopback-only TCP port the bundled nginx listens on.
    /// Default: 0 = auto (a second ephemeral loopback port allocation). Must
    /// differ from BOTH <see cref="ListenPort"/> (public Kestrel) AND the
    /// raw-origin port (<see cref="OriginPort"/>) — three distinct sockets live
    /// in one process. Ignored in Process/External modes.
    /// </summary>
    public int NginxLoopbackPort { get; set; } = 0;

    /// <summary>
    /// Inverse only: when <c>true</c> (the DEFAULT), the package binds the PUBLIC
    /// Kestrel endpoint on <c>IPAddress.Any:</c><see cref="ListenPort"/> itself, so
    /// the public port and the private raw-origin are BOTH code-configured endpoints
    /// and bind reliably. Set <c>false</c> ONLY if you configure the public endpoint
    /// yourself via <c>Kestrel:Endpoints</c> — note that <c>--urls</c>/<c>UseUrls</c>/
    /// <c>ASPNETCORE_URLS</c> hosting URLs are OVERRIDDEN once the sidecar adds its
    /// loopback raw-origin endpoint (Kestrel's <c>PreferHostingUrls=false</c>), so
    /// those do NOT bind the public port in this mode; use <c>Kestrel:Endpoints</c>
    /// or leave this <c>true</c>. For edge-TLS, front the package's public port with
    /// a TLS terminator / load balancer. Ignored in Process/External modes.
    /// </summary>
    public bool OwnPublicPort { get; set; } = true;

    /// <summary>
    /// Inverse only: when <c>true</c>, the middleware optimizes ONLY requests whose
    /// Host is in <see cref="DomainOptions.AuthorizedDomains"/> (loopback —
    /// localhost/127.0.0.1 — is always allowed regardless); every other host is
    /// served un-optimized. Default <c>false</c> = forward ALL hosts (zero-config
    /// same-origin optimization — the module implicitly authorizes each request's own
    /// same-origin resources with no domain configuration). This is a defense-in-depth
    /// knob for operators who want to pin which hosts are optimized; the Host-keyed
    /// cache surface is bounded (size-capped, Host-fragmented caches) either way, so
    /// the default forward-all matches running nginx+pagespeed in front of the app.
    /// Ignored in Process/External modes.
    /// </summary>
    public bool RestrictToAuthorizedHosts { get; set; } = false;

    /// <summary>
    /// Inverse only: opt-in to forward the five admin/diagnostic handler paths
    /// (/pagespeed_admin, /pagespeed_statistics, /pagespeed_global_statistics,
    /// /pagespeed_message, /pagespeed_console) from the PUBLIC Kestrel front door.
    /// Default: <c>false</c> — those paths return 404 from the public endpoint.
    /// In Inverse the module's real-TCP-peer IP-ACL is nullified (nginx always
    /// sees the loopback middleware), so the bearer is load-bearing: enabling
    /// this REQUIRES <see cref="PageSpeedOptions.AdminAuth"/>.Enabled (the
    /// validator already hard-rejects Inverse + AdminAuth disabled). Ignored in
    /// Process/External modes (nginx is the front door there).
    /// </summary>
    public bool AllowPublicAdmin { get; set; } = false;

    /// <summary>
    /// Path to the bundled nginx binary. If not set, searches
    /// AppContext.BaseDirectory first (the bundled runtimes/{rid}/native/ layout),
    /// then well-known locations and PATH. The bundled binary is probed first so the
    /// nginx that matches the bundled module always wins over any system nginx.
    /// </summary>
    public string? BinaryPath { get; set; }

    /// <summary>
    /// Path to the matched ngx_pagespeed_module.so. If not set, resolves to
    /// "ngx_pagespeed_module.so" next to the nginx binary (the bundled matched
    /// pair). nginx refuses to load a module built against any other nginx version,
    /// down to the patch level, so the two halves always travel and resolve together.
    /// </summary>
    public string? ModulePath { get; set; }

    /// <summary>
    /// Directory for the generated nginx config and runtime files (pid, error
    /// log). Default: a private per-app directory under the system temp dir.
    /// </summary>
    public string? ConfigDirectory { get; set; }

    /// <summary>
    /// Launch the bundled nginx through the native <c>pagespeed-nginx-launch</c> shim,
    /// which sets <c>PR_SET_PDEATHSIG</c> so a hard SIGKILL/OOM-kill/container-hard-stop
    /// of the host process cannot orphan nginx once launch has settled (an orphaned
    /// nginx would reparent to init and keep holding its loopback listen port). Default
    /// <c>true</c>. The parent-death signal tracks the *launching thread*, so the package
    /// forks nginx on a single long-lived launch thread that lives for the whole manager
    /// lifetime — the signal then fires only on real host-process death. (The earlier
    /// model launched from a threadpool thread that retired moments later and falsely
    /// SIGTERM'd nginx ~1s after start; that is fixed.) Normal shutdown still stops nginx
    /// gracefully (StopAsync runs <c>nginx -s quit</c>; Dispose kills the process tree);
    /// the shim is the orphan-on-hard-kill backstop on top. Effective on Linux only AND
    /// only when the bundled shim binary sits next to nginx — older/mixed NativeAssets
    /// packages without it degrade gracefully (nginx is launched directly). Set
    /// <c>false</c> to force the direct launch.
    /// </summary>
    public bool UseLaunchShim { get; set; } = true;

    /// <summary>
    /// Whether to automatically restart nginx on crash. Default: true.
    /// </summary>
    public bool AutoRestart { get; set; } = true;

    /// <summary>
    /// Maximum number of restart attempts before giving up. Default: 5.
    /// </summary>
    public int MaxRestartAttempts { get; set; } = 5;

    /// <summary>
    /// Delay between restart attempts in milliseconds. Default: 1000.
    /// </summary>
    public int RestartDelayMs { get; set; } = 1000;

    /// <summary>
    /// Health check poll interval in milliseconds. Default: 5000.
    /// </summary>
    public int HealthCheckIntervalMs { get; set; } = 5000;

    /// <summary>
    /// Timeout for health check requests in milliseconds. Default: 2000.
    /// </summary>
    public int HealthCheckTimeoutMs { get; set; } = 2000;

    /// <summary>
    /// Startup timeout in milliseconds. Default: 30000.
    /// </summary>
    public int StartupTimeoutMs { get; set; } = 30000;

    /// <summary>
    /// Graceful shutdown timeout in milliseconds. Default: 10000.
    /// </summary>
    public int ShutdownTimeoutMs { get; set; } = 10000;

    /// <summary>
    /// Escape-hatch environment variables to set on the nginx child process. Values
    /// reach the child via the process environment dictionary directly (no shell, no
    /// injection surface). The package adds no entries of its own.
    /// </summary>
    public Dictionary<string, string> EnvironmentVariables { get; set; } = [];
}
