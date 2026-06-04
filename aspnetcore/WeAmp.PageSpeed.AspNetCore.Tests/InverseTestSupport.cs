using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Yarp.ReverseProxy.Forwarder;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Shared fixtures for the Tier-1 (nginx-free) Inverse-mode tests: a capturing
/// stub <see cref="IPageSpeedForwarder"/>, a fake <see cref="ISidecarManager"/>,
/// and an <see cref="HttpContext"/> builder that pins the public/raw-origin
/// signals (LocalPort, the IRawOriginMarker feature, transport peer).
/// </summary>
internal static class InverseTestSupport
{
    public const int PublicPort = 8080;
    public const int RawOriginPort = 5000;
    public const int NginxLoopbackPort = 5100;

    /// <summary>
    /// Builds an InternalSidecarEndpoint pinned exactly as the Inverse raw-origin
    /// configurator would (raw-origin loopback-TCP port + nginx loopback port).
    /// </summary>
    public static InternalSidecarEndpoint NewEndpoint(
        int rawOriginPort = RawOriginPort, int nginxLoopbackPort = NginxLoopbackPort)
    {
        var ep = new InternalSidecarEndpoint();
        ep.SetPort(rawOriginPort);
        ep.SetRawOriginLoopbackPort(rawOriginPort);
        ep.SetNginxLoopbackPort(nginxLoopbackPort);
        return ep;
    }

    public static PageSpeedInverseMiddleware NewMiddleware(
        IPageSpeedForwarder forwarder,
        InternalSidecarEndpoint endpoint,
        PageSpeedOptions? options = null,
        ISidecarManager? sidecar = null,
        RequestDelegate? next = null)
    {
        // Default test options leave RestrictToAuthorizedHosts = false (forward-all),
        // so the generic classification/forward tests are Host-independent (the
        // strict allowlist has its own dedicated tests in InverseHardeningTests).
        options ??= new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse },
            Domains = new DomainOptions(),
        };
        sidecar ??= new FakeSidecarManager { State = SidecarState.Running };
        next ??= ctx => { ctx.Items["__next_called"] = true; return Task.CompletedTask; };

        return new PageSpeedInverseMiddleware(
            next,
            Microsoft.Extensions.Options.Options.Create(options),
            sidecar,
            endpoint,
            forwarder,
            NullLogger<PageSpeedInverseMiddleware>.Instance);
    }

    /// <summary>
    /// Builds an HttpContext for a request. <paramref name="rawOrigin"/> sets the
    /// raw-origin signals (LocalPort == raw-origin port AND the IRawOriginMarker
    /// feature); otherwise the request looks like it arrived on the public port.
    /// </summary>
    public static DefaultHttpContext NewContext(
        string method = "GET",
        string path = "/",
        string scheme = "https",
        // A plain default host; under forward-all this is forwarded regardless. (The
        // strict-allowlist + loopback-always tests in InverseHardeningTests pass their
        // own host, and the transformer header tests pass their own host directly.)
        string host = "localhost",
        bool rawOrigin = false,
        bool setMarker = false,
        int? localPort = null,
        string remoteIp = "127.0.0.1",
        IDictionary<string, string>? headers = null)
    {
        var ctx = new DefaultHttpContext();
        ctx.Request.Method = method;
        ctx.Request.Path = path;
        ctx.Request.Scheme = scheme;
        ctx.Request.Host = new HostString(host);

        ctx.Connection.RemoteIpAddress = string.IsNullOrEmpty(remoteIp)
            ? null
            : System.Net.IPAddress.Parse(remoteIp);
        ctx.Connection.LocalPort = localPort ?? (rawOrigin ? RawOriginPort : PublicPort);

        if (rawOrigin || setMarker)
        {
            ctx.Features.Set<IRawOriginMarker>(RawOriginMarker.Instance);
        }

        if (headers != null)
        {
            foreach (var (k, v) in headers)
            {
                ctx.Request.Headers[k] = v;
            }
        }

        // A throwaway response body so writes don't NRE.
        ctx.Response.Body = new MemoryStream();
        return ctx;
    }

    public static bool NextWasCalled(HttpContext ctx) =>
        ctx.Items.TryGetValue("__next_called", out var v) && v is true;
}

/// <summary>
/// Capturing stub forwarder: records every SendAsync (destination + a snapshot of
/// the inbound request headers) and returns a configurable
/// <see cref="ForwarderError"/>. Optionally marks the response started so the
/// middleware's after-start abort path can be exercised.
/// </summary>
internal sealed class StubPageSpeedForwarder : IPageSpeedForwarder
{
    public int SendCount { get; private set; }
    public string? LastDestination { get; private set; }
    public List<HttpContext> Contexts { get; } = new();
    public ForwarderError ResultToReturn { get; set; } = ForwarderError.None;
    public Action<HttpContext>? OnSend { get; set; }

    public ValueTask<ForwarderError> SendAsync(
        HttpContext context, string destinationPrefix, CancellationToken cancellationToken = default)
    {
        SendCount++;
        LastDestination = destinationPrefix;
        Contexts.Add(context);
        OnSend?.Invoke(context);
        return ValueTask.FromResult(ResultToReturn);
    }
}

internal sealed class FakeSidecarManager : ISidecarManager
{
    public SidecarState State { get; set; } = SidecarState.Running;
    public string? AdminToken { get; set; }
    public string? ErrorMessage { get; set; }
    public int RestartCount { get; set; }
#pragma warning disable CS0067
    public event EventHandler<SidecarStateChangedEventArgs>? StateChanged;
#pragma warning restore CS0067
    public Task StartAsync(CancellationToken cancellationToken = default) => Task.CompletedTask;
    public Task StopAsync(CancellationToken cancellationToken = default) => Task.CompletedTask;
    public Task<bool> CheckHealthAsync(CancellationToken cancellationToken = default) => Task.FromResult(true);
}
