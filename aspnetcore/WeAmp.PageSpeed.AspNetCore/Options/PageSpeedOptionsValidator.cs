// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;

namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Eagerly-run validation for <see cref="PageSpeedOptions"/>, wired through the
/// <c>.ValidateOnStart()</c> calls in the DI extensions. Without a registered
/// <see cref="IValidateOptions{T}"/> those calls validate nothing; this catches
/// misconfigurations at host build with an actionable message instead of an opaque
/// nginx startup crash or a silently-ignored value.
/// </summary>
internal sealed class PageSpeedOptionsValidator : IValidateOptions<PageSpeedOptions>
{
    private readonly ILogger _logger;

    // Parameterless ctor keeps the existing tests' `new PageSpeedOptionsValidator()`
    // working; DI supplies a real logger so non-fatal warnings (e.g. Inverse with no
    // OwnPublicPort) reach the operator.
    public PageSpeedOptionsValidator()
        : this(NullLogger<PageSpeedOptionsValidator>.Instance)
    {
    }

    public PageSpeedOptionsValidator(ILogger<PageSpeedOptionsValidator> logger)
    {
        _logger = logger;
    }

    public ValidateOptionsResult Validate(string? name, PageSpeedOptions options)
    {
        var errors = new List<string>();
        var sc = options.Sidecar;

        // Docker mode is reserved for a future release — fail clearly
        // rather than letting it fall through to the Process spawn path.
        if (sc.Mode == SidecarMode.Docker)
        {
            errors.Add(
                "PageSpeed:Sidecar:Mode 'Docker' is reserved for a future release and is not implemented; " +
                "use 'Process' (bundled nginx child process) or 'External' (operator-managed nginx).");
        }

        if (sc.ListenPort is < 1 or > 65535)
        {
            errors.Add($"PageSpeed:Sidecar:ListenPort {sc.ListenPort} is out of range (1-65535).");
        }

        // OriginPort: 0 = auto (Unix socket / ephemeral loopback); otherwise a valid
        // loopback port. A negative value would otherwise be silently coerced to auto.
        if (sc.OriginPort != 0 && sc.OriginPort is < 1 or > 65535)
        {
            errors.Add(
                $"PageSpeed:Sidecar:OriginPort {sc.OriginPort} is out of range (use 0 for auto, or 1-65535).");
        }

        // nginx (public front-end) and Kestrel (loopback origin) cannot share a port.
        // Valid to enforce for Process; in Inverse the three-way distinctness check
        // below subsumes this.
        if (sc.Mode == SidecarMode.Process && sc.OriginPort > 0 && sc.OriginPort == sc.ListenPort)
        {
            errors.Add(
                $"PageSpeed:Sidecar:OriginPort must differ from ListenPort ({sc.ListenPort}); the nginx front-end " +
                "and the Kestrel origin cannot bind the same port.");
        }

        if (sc.MaxRestartAttempts < 0)
        {
            errors.Add($"PageSpeed:Sidecar:MaxRestartAttempts {sc.MaxRestartAttempts} must be >= 0.");
        }

        if (sc.Mode == SidecarMode.Inverse)
        {
            ValidateInverse(options, sc, errors);
        }

        return errors.Count == 0
            ? ValidateOptionsResult.Success
            : ValidateOptionsResult.Fail(errors);
    }

    /// <summary>
    /// Inverse-mode rules. The default flip to Inverse is only
    /// safe because these close the two blockers by construction: the loop break
    /// needs an authoritative loopback-TCP LocalPort (reject UDS raw origin), and
    /// the module IP-ACL is nullified so the bearer is load-bearing (reject
    /// AdminAuth disabled). Three sockets live in one process → three-way port
    /// distinctness. Linux-only ELF .so → reject off-Linux.
    /// </summary>
    private void ValidateInverse(PageSpeedOptions options, SidecarOptions sc, List<string> errors)
    {
        // (6) Linux-only: the bundled ngx_pagespeed .so is an ELF; mirror the
        // fail-closed philosophy off-Linux.
        if (!OperatingSystem.IsLinux())
        {
            errors.Add(
                "PageSpeed:Sidecar:Mode 'Inverse' is Linux-only (the bundled nginx + ngx_pagespeed is a Linux ELF " +
                "module). On Windows use the IIS module; on macOS/containers without nginx use ModPageSpeed 2.0 " +
                "(WeAmp.PageSpeed.AspNetCore).");
        }

        // (1) NginxLoopbackPort range (0 = auto, else 1-65535).
        if (sc.NginxLoopbackPort != 0 && sc.NginxLoopbackPort is < 1 or > 65535)
        {
            errors.Add(
                $"PageSpeed:Sidecar:NginxLoopbackPort {sc.NginxLoopbackPort} is out of range (use 0 for auto, or 1-65535).");
        }

        // (2) Three-way distinctness: public Kestrel (ListenPort), private nginx
        // listen (NginxLoopbackPort), private raw-origin loopback-TCP (OriginPort).
        // Only compare explicitly-set (non-zero) ports — auto-allocated ports cannot
        // collide by construction.
        if (sc.NginxLoopbackPort > 0 && sc.NginxLoopbackPort == sc.ListenPort)
        {
            errors.Add(
                $"PageSpeed:Sidecar:NginxLoopbackPort must differ from ListenPort ({sc.ListenPort}) in Inverse mode " +
                "(the public Kestrel port and the private nginx listen port cannot collide).");
        }
        if (sc.NginxLoopbackPort > 0 && sc.OriginPort > 0 && sc.NginxLoopbackPort == sc.OriginPort)
        {
            errors.Add(
                $"PageSpeed:Sidecar:NginxLoopbackPort must differ from OriginPort ({sc.OriginPort}) in Inverse mode " +
                "(the private nginx listen port and the raw-origin loopback-TCP port cannot collide).");
        }
        if (sc.OriginPort > 0 && sc.OriginPort == sc.ListenPort)
        {
            errors.Add(
                $"PageSpeed:Sidecar:OriginPort must differ from ListenPort ({sc.ListenPort}) in Inverse mode " +
                "(the public Kestrel port and the raw-origin loopback-TCP port cannot collide).");
        }

        // (3) AdminAuth bearer is load-bearing in Inverse: the module's real-TCP-peer
        // IP-ACL always sees the loopback middleware, so the IP-ACL no longer gates
        // admin. The bearer may not be disabled.
        if (!options.AdminAuth.Enabled)
        {
            errors.Add(
                "PageSpeed:AdminAuth:Enabled must be true in Inverse mode. The nginx module decides admin access " +
                "from the real TCP peer IP, which in Inverse is ALWAYS the loopback middleware, so the IP-ACL " +
                "always admits and the bearer is the only effective admin gate (it may not be disabled).");
        }

        // (4) UDS raw origin is rejected: the loop break needs an authoritative
        // loopback-TCP LocalPort. A non-null SocketPath would request a UDS.
        if (sc.SocketPath != null && sc.SocketPath.Length > 0)
        {
            errors.Add(
                "PageSpeed:Sidecar:SocketPath (a Unix-domain raw origin) is not allowed in Inverse mode. The loop " +
                "break is keyed on the kernel-set raw-origin loopback-TCP LocalPort, which a UDS connection does " +
                "not provide (LocalPort is 0). Leave SocketPath unset and use OriginPort (0 = auto loopback-TCP).");
        }

        // (5) WARN (non-fatal) when the operator opted OUT of the package owning the
        // public bind. In that mode the public endpoint MUST be configured via
        // Kestrel:Endpoints — --urls/UseUrls/ASPNETCORE_URLS hosting URLs are
        // overridden once the raw-origin loopback endpoint is added (Kestrel
        // PreferHostingUrls=false), so they would silently fail to bind publicly.
        if (!sc.OwnPublicPort)
        {
            _logger.LogWarning(
                "PageSpeed Inverse: Sidecar.OwnPublicPort is false, so the package will NOT bind a public " +
                "Kestrel endpoint. Configure the public endpoint yourself via Kestrel:Endpoints — note that " +
                "--urls/UseUrls/ASPNETCORE_URLS are OVERRIDDEN once the sidecar adds its loopback endpoint and " +
                "will NOT bind publicly. Or leave Sidecar.OwnPublicPort = true (default) to bind 0.0.0.0:{Port}.",
                sc.ListenPort);
        }

        // (7) WARN (non-fatal) ONLY when the operator opted into the strict allowlist
        // but left just the loopback seed: every non-loopback host would then be served
        // un-optimized. The DEFAULT (forward-all) path imposes NO such obligation — this
        // never fires unless RestrictToAuthorizedHosts was explicitly enabled.
        if (sc.RestrictToAuthorizedHosts && OnlyLoopbackSeed(options.Domains?.AuthorizedDomains))
        {
            _logger.LogWarning(
                "PageSpeed Inverse: Sidecar.RestrictToAuthorizedHosts is true but Domains.AuthorizedDomains " +
                "contains only the loopback seed, so every public host will be served UN-optimized until you add " +
                "your host(s). Add them to Domains.AuthorizedDomains, or leave RestrictToAuthorizedHosts = false " +
                "(the default) to optimize all hosts.");
        }
    }

    /// <summary>
    /// True when the authorized-domains list is empty or contains only the harmless
    /// loopback seed (localhost / 127.0.0.1 / ::1) — i.e. no real public host. Used to
    /// surface the strict-mode "nothing public is optimized" warning.
    /// </summary>
    private static bool OnlyLoopbackSeed(List<string>? domains)
    {
        if (domains is null || domains.Count == 0) return true;
        foreach (var d in domains)
        {
            var h = d?.Trim();
            if (string.IsNullOrEmpty(h)) continue;
            if (!string.Equals(h, "localhost", StringComparison.OrdinalIgnoreCase)
                && h != "127.0.0.1" && h != "::1" && h != "[::1]")
            {
                return false;
            }
        }
        return true;
    }
}
