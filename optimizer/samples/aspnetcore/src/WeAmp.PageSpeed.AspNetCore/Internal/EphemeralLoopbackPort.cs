// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using System.Net.Sockets;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Allocates a free TCP port on the loopback interface by binding a
/// <see cref="TcpListener"/> with port 0 and immediately closing it.
/// Used to hand a kernel-assigned port to the worker child process
/// when <see cref="WorkerOptions.ApiPort"/> is 0 (auto-allocate).
/// </summary>
/// <remarks>
/// Classic TOCTOU caveat: another process can race in and grab the
/// port between the close and the worker's <c>bind()</c>. For our use
/// case — a child process started microseconds later, binding the
/// same loopback IP — the window is negligible. We never bind on
/// <see cref="IPAddress.Any"/>; loopback only.
/// </remarks>
internal static class EphemeralLoopbackPort
{
    /// <summary>
    /// Returns a free port on 127.0.0.1.
    /// </summary>
    public static int Allocate()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        try
        {
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }
}
