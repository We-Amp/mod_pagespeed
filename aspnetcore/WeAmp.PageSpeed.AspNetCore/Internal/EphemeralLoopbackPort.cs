using System.Net;
using System.Net.Sockets;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Allocates a free TCP port on the loopback interface by binding a
/// <see cref="TcpListener"/> with port 0 and immediately closing it.
/// Used to hand a kernel-assigned port to the Kestrel origin / nginx
/// sidecar rendezvous when the configured origin port is 0 (auto-allocate)
/// and a Unix-domain socket cannot be used.
/// </summary>
/// <remarks>
/// Classic TOCTOU caveat: another process can race in and grab the
/// port between the close and the subsequent <c>bind()</c>. For our use
/// case — Kestrel binds the same loopback IP microseconds later and the
/// spawned nginx connects to it — the window is negligible. We never bind on
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
