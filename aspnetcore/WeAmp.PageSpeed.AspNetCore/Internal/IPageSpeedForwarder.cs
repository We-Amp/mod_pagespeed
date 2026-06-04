using Microsoft.AspNetCore.Http;
using Yarp.ReverseProxy.Forwarder;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Testability seam over YARP's <see cref="IHttpForwarder"/>: the Inverse
/// middleware's loop-break / classify / header logic can be unit-tested with a
/// stub implementation (no live nginx). The default implementation
/// (<see cref="HttpForwarderAdapter"/>) wraps <see cref="IHttpForwarder"/> with a
/// singleton <see cref="System.Net.Http.HttpMessageInvoker"/> for the loopback
/// transport and the <see cref="InverseForwardTransformer"/>.
/// </summary>
public interface IPageSpeedForwarder
{
    /// <summary>
    /// Streams the current request to <paramref name="destinationPrefix"/>
    /// (the bundled nginx loopback endpoint) and relays the response back, with
    /// no buffering. Returns the YARP <see cref="ForwarderError"/>
    /// (<see cref="ForwarderError.None"/> on success) so the caller can choose
    /// graceful degradation (fall through to _next when nothing has been written)
    /// vs. abort (after the response has started).
    /// </summary>
    ValueTask<ForwarderError> SendAsync(
        HttpContext context,
        string destinationPrefix,
        CancellationToken cancellationToken = default);
}
