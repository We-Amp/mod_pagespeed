// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore.Sidecar;

/// <summary>
/// Represents the current state of the PageSpeed sidecar.
/// </summary>
public enum SidecarState
{
    /// <summary>
    /// Sidecar has not been started.
    /// </summary>
    NotStarted,

    /// <summary>
    /// Sidecar is starting up.
    /// </summary>
    Starting,

    /// <summary>
    /// Sidecar is running and healthy.
    /// </summary>
    Running,

    /// <summary>
    /// Sidecar is running but health check failed.
    /// </summary>
    Unhealthy,

    /// <summary>
    /// Sidecar has stopped unexpectedly and is restarting.
    /// </summary>
    Restarting,

    /// <summary>
    /// Sidecar has stopped.
    /// </summary>
    Stopped,

    /// <summary>
    /// Sidecar failed to start or exceeded restart attempts.
    /// </summary>
    Failed
}
