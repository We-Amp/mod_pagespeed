// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Configuration for the embedded /console/ SPA and its proxy to the
/// worker's API. The SPA is shipped inside the package's main
/// assembly as embedded resources and served by <see cref="ConsoleMiddleware"/>.
/// </summary>
public sealed class ConsoleOptions
{
    /// <summary>
    /// URL path the SPA is mounted at. Default: <c>/console</c>.
    /// Customers can re-mount to avoid colliding with their own routes,
    /// e.g. <c>/_pagespeed/console</c>.
    /// </summary>
    public string MountPath { get; set; } = "/console";

    /// <summary>
    /// Whether the embedded console and its worker proxy are enabled.
    /// When false, the middleware passes every request through to the
    /// next delegate. Default: <c>true</c>.
    /// </summary>
    public bool Enabled { get; set; } = true;

    /// <summary>
    /// Advisory: when true and the request is not over HTTPS, the
    /// middleware returns 403 for the console and worker-proxy paths.
    /// Default: <c>false</c>.
    /// </summary>
    public bool RequireHttps { get; set; } = false;
}
