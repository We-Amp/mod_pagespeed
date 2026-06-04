using Microsoft.AspNetCore.Http;
using Yarp.ReverseProxy.Forwarder;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Default <see cref="IPageSpeedForwarder"/>: wraps YARP's
/// <see cref="IHttpForwarder"/> with a singleton loopback
/// <see cref="HttpMessageInvoker"/> (see
/// <c>PageSpeedServiceCollectionExtensions</c> for its
/// <see cref="System.Net.Http.SocketsHttpHandler"/> configuration:
/// AllowAutoRedirect=false, UseCookies=false, AutomaticDecompression=None) and
/// the <see cref="InverseForwardTransformer"/>. Pure streaming relay — no
/// buffering — so the bundled nginx (not the middleware) decides what to
/// optimize from the upstream Content-Type.
/// </summary>
internal sealed class HttpForwarderAdapter : IPageSpeedForwarder
{
    private readonly IHttpForwarder _forwarder;
    private readonly HttpMessageInvoker _invoker;
    private readonly InverseForwardTransformer _transformer;
    private readonly ForwarderRequestConfig _requestConfig;

    public HttpForwarderAdapter(
        IHttpForwarder forwarder,
        LoopbackHttpMessageInvoker invoker,
        InverseForwardTransformer transformer)
    {
        _forwarder = forwarder;
        _invoker = invoker.Invoker;
        _transformer = transformer;
        _requestConfig = ForwarderRequestConfig.Empty;
    }

    public ValueTask<ForwarderError> SendAsync(
        HttpContext context,
        string destinationPrefix,
        CancellationToken cancellationToken = default) =>
        _forwarder.SendAsync(context, destinationPrefix, _invoker, _requestConfig, _transformer, cancellationToken);
}

/// <summary>
/// Singleton holder for the loopback <see cref="HttpMessageInvoker"/> used by
/// <see cref="HttpForwarderAdapter"/>. Registered as a singleton so the
/// connection pool is reused across requests (YARP's documented guidance — never
/// create a new invoker per request). Disposes the underlying handler on host
/// teardown.
/// </summary>
internal sealed class LoopbackHttpMessageInvoker : IDisposable
{
    public HttpMessageInvoker Invoker { get; }

    public LoopbackHttpMessageInvoker(TimeSpan connectTimeout)
    {
        var handler = new SocketsHttpHandler
        {
            // YARP guidance for IHttpForwarder: disable everything the destination
            // (the bundled nginx) does not need; let the transformer/headers carry
            // the contract.
            UseProxy = false,
            AllowAutoRedirect = false,
            AutomaticDecompression = System.Net.DecompressionMethods.None,
            UseCookies = false,
            ActivityHeadersPropagator = null,
            ConnectTimeout = connectTimeout,
            // Modest lifetime so a recycled nginx (restart) is not pinned to a
            // dead connection indefinitely.
            PooledConnectionLifetime = TimeSpan.FromMinutes(2),
        };
        Invoker = new HttpMessageInvoker(handler, disposeHandler: true);
    }

    public void Dispose() => Invoker.Dispose();
}
