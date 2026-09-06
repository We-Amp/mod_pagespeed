// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Options;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The PUBLIC-bind half of the Inverse-mode Kestrel split (review fix: kept
/// SEPARATE from the raw-origin configurator in
/// <see cref="SidecarKestrelConfigureOptions"/> so a bug in one cannot widen the
/// other). Active only when PageSpeed is Enabled AND Sidecar.Mode == Inverse.
///
/// Binds the public port itself when <c>Sidecar.OwnPublicPort</c> is true (the
/// DEFAULT) so the public endpoint and the raw-origin are BOTH code-configured
/// and bind reliably. When <c>OwnPublicPort = false</c> the operator must supply
/// the public endpoint via <c>Kestrel:Endpoints</c> — <c>--urls</c>/<c>UseUrls</c>/
/// <c>ASPNETCORE_URLS</c> hosting URLs are overridden once the raw-origin
/// loopback endpoint is added (Kestrel <c>PreferHostingUrls=false</c>), so they do
/// NOT bind publicly in that mode.
///
/// This NEVER touches the raw-origin listener and NEVER binds the raw origin on
/// <see cref="IPAddress.Any"/> — it only ever binds the PUBLIC port the host's
/// own request pipeline (with <c>UsePageSpeed()</c>) terminates.
/// </summary>
internal sealed class PublicKestrelConfigureOptions : IConfigureOptions<KestrelServerOptions>
{
    private readonly IOptions<PageSpeedOptions> _options;
    private readonly ILogger<PublicKestrelConfigureOptions> _logger;

    public PublicKestrelConfigureOptions(
        IOptions<PageSpeedOptions> options,
        ILogger<PublicKestrelConfigureOptions> logger)
    {
        _options = options;
        _logger = logger;
    }

    public void Configure(KestrelServerOptions kestrel)
    {
        var ps = _options.Value;
        if (!ps.Enabled || ps.Sidecar.Mode != SidecarMode.Inverse)
        {
            return;
        }

        if (!ps.Sidecar.OwnPublicPort)
        {
            // Operator supplies the public endpoint via Kestrel:Endpoints (NOT
            // --urls/UseUrls, which are overridden once the raw origin is added).
            _logger.LogDebug(
                "Inverse: Sidecar.OwnPublicPort is false; leaving the public Kestrel endpoint to Kestrel:Endpoints.");
            return;
        }

        // Auto-own the public port. IPAddress.Any here is the PUBLIC front door
        // (Kestrel terminates client traffic) — distinct from the raw origin,
        // which SidecarKestrelConfigureOptions binds loopback-only.
        kestrel.Listen(IPAddress.Any, ps.Sidecar.ListenPort);
        _logger.LogDebug("Inverse: auto-bound the public Kestrel endpoint on 0.0.0.0:{Port}", ps.Sidecar.ListenPort);
    }
}
