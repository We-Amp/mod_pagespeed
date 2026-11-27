// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.AspNetCore.Http.Metadata;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.Routing.Matching;
using Microsoft.AspNetCore.Routing.Patterns;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The anonymous request the middleware stores instead of the visitor's
/// response.
/// </summary>
/// <remarks>
/// <para>A stored response is served from the cache, and used to build other
/// pages, for every client of its URL. So on a cache miss whose response would
/// be stored, the middleware asks the pipeline after it for the same URL once
/// more, in process, and stores that answer, not the visitor's: the same
/// method, scheme, host name, path and query, from the client address
/// <see cref="AgentSelfFetch.Label"/> (224.0.0.0, an address no client
/// connects from), with no signed-in user and none of the request's other
/// headers apart from <c>Accept</c>, <c>Accept-Language</c> and
/// <c>User-Agent</c>.</para>
///
/// <para>The request is the one that reached the middleware, before the rest
/// of the pipeline could rewrite it. It carries no port and no local address:
/// the cache key does not include them, so what is stored must not depend on
/// the listener the visitor reached. Endpoint and route values are carried
/// over only when they were set before the middleware ran; otherwise the
/// pipeline selects them for this request itself. Nothing is stored for an
/// endpoint set before it whose selection may depend on the client
/// (<see cref="EndpointDependsOnClient"/>).</para>
///
/// <para>The answer is used only when it has the visitor's status and media
/// type. An exception, or a cancelled request, counts as a different
/// answer.</para>
///
/// <para>Same shape as the worker's sub-resource check
/// (src/worker/subresource_gate.h), which asks the site with an anonymous
/// request before it uses another URL's cached bytes.</para>
/// </remarks>
internal static class AnonymousFetchCheck
{
    // Request headers the check carries over: content negotiation only.
    private static readonly string[] CarriedHeaders =
    {
        "Accept", "Accept-Language", "User-Agent",
    };

    /// <summary>
    /// The request as it reached the middleware, before the rest of the
    /// pipeline ran (and could rewrite its path or select an endpoint for it):
    /// its request line, the carried headers, and the endpoint and route values
    /// when they were already set.
    /// </summary>
    internal sealed record Seed(
        string Protocol, string Method, string Scheme, string Host,
        string PathBase, string Path, string QueryString,
        KeyValuePair<string, Microsoft.Extensions.Primitives.StringValues>[] Headers,
        Endpoint? Endpoint, RouteValueDictionary? RouteValues)
    {
        /// <summary>Reads <paramref name="context"/>'s request as it is now.</summary>
        public static Seed Capture(HttpContext context)
        {
            var request = context.Request;
            var headers = new List<KeyValuePair<string, Microsoft.Extensions.Primitives.StringValues>>();
            foreach (var name in CarriedHeaders)
            {
                if (request.Headers.TryGetValue(name, out var value))
                    headers.Add(new(name, value));
            }
            var endpoint = context.GetEndpoint();
            return new Seed(
                request.Protocol, request.Method, request.Scheme,
                request.Host.HasValue ? request.Host.Host : string.Empty,
                request.PathBase.Value ?? string.Empty,
                request.Path.Value ?? string.Empty,
                request.QueryString.Value ?? string.Empty,
                headers.ToArray(),
                endpoint,
                endpoint == null ? null : new RouteValueDictionary(request.RouteValues));
        }
    }

    /// <summary>
    /// Whether <paramref name="endpoint"/>, selected before the middleware
    /// ran, may have been selected or admitted by something about the client
    /// that the anonymous request does not share: then the anonymous request,
    /// which reuses it, would be answered as the visitor was. True when
    /// anything about it cannot be told, so the default is to refuse:
    /// <list type="bullet">
    /// <item>host or port metadata (<c>RequireHost</c>), request content type
    /// metadata, or authorization metadata;</item>
    /// <item>any metadata of a type outside the framework;</item>
    /// <item>a route constraint outside the framework, or one that cannot be
    /// resolved;</item>
    /// <item>a matcher policy other than the HTTP method policy that applies
    /// to the endpoint, or one whose kind is unknown;</item>
    /// <item>no services to resolve the matcher policies from.</item>
    /// </list>
    /// </summary>
    internal static bool EndpointDependsOnClient(Endpoint? endpoint, IServiceProvider? services)
    {
        if (endpoint == null) return false;
        if (services == null) return true;
        try
        {
            var metadata = endpoint.Metadata;
            if (metadata.GetMetadata<IHostMetadata>() is { Hosts.Count: > 0 } ||
                metadata.GetMetadata<IAcceptsMetadata>() != null ||
                metadata.GetMetadata<IAuthorizeData>() != null ||
                metadata.GetMetadata<AuthorizationPolicy>() != null ||
                metadata.GetMetadata<IAuthorizationRequirementData>() != null)
                return true;
            foreach (var item in metadata)
            {
                if (!IsFrameworkType(item.GetType())) return true;
            }

            if (endpoint is RouteEndpoint route &&
                !RouteConstraintsAreFramework(route.RoutePattern, services))
                return true;

            var single = new[] { endpoint };
            foreach (var policy in services.GetServices<MatcherPolicy>())
            {
                if (policy is HttpMethodMatcherPolicy) continue;  // the method is shared
                bool known = false, applies = false;
                if (policy is IEndpointSelectorPolicy selector)
                {
                    known = true;
                    applies |= selector.AppliesToEndpoints(single);
                }
                if (policy is INodeBuilderPolicy nodeBuilder)
                {
                    known = true;
                    applies |= nodeBuilder.AppliesToEndpoints(single);
                }
                if (policy is IEndpointComparerPolicy) known = true;
                if (!known || applies) return true;
            }
            return false;
        }
        catch
        {
            return true;
        }
    }

    [UnconditionalSuppressMessage("Trimming", "IL2026",
        Justification = "The constraint map is only read, to name the type behind a constraint.")]
    private static bool RouteConstraintsAreFramework(RoutePattern pattern, IServiceProvider services)
    {
        if (pattern.ParameterPolicies.Count == 0) return true;
        var map = services.GetService<IOptions<RouteOptions>>()?.Value.ConstraintMap;
        foreach (var references in pattern.ParameterPolicies.Values)
        {
            foreach (var reference in references)
            {
                if (reference.ParameterPolicy != null)
                {
                    if (!IsFrameworkType(reference.ParameterPolicy.GetType())) return false;
                    continue;
                }
                var content = reference.Content ?? string.Empty;
                var paren = content.IndexOf('(');
                var name = paren < 0 ? content : content[..paren];
                if (map == null || !map.TryGetValue(name, out var type) || !IsFrameworkType(type))
                    return false;
            }
        }
        return true;
    }

    // Types shipped with .NET or ASP.NET Core (what a framework matcher or
    // middleware can be known to read).
    private static bool IsFrameworkType(Type type)
    {
        var name = type.Assembly.GetName().Name ?? string.Empty;
        return name.StartsWith("Microsoft.AspNetCore", StringComparison.Ordinal) ||
               name.StartsWith("Microsoft.Extensions.", StringComparison.Ordinal) ||
               name.StartsWith("System.", StringComparison.Ordinal) ||
               name is "System" or "mscorlib" or "netstandard";
    }

    /// <summary>
    /// The anonymous request for <paramref name="seed"/>, answering
    /// <paramref name="context"/>'s visitor, with its body written to
    /// <paramref name="body"/>. Run it with <see cref="RunAsync"/>.
    /// </summary>
    internal static DefaultHttpContext Create(
        HttpContext context, Seed seed, Stream body)
    {
        var requestFeature = new HttpRequestFeature
        {
            Protocol = seed.Protocol,
            Method = seed.Method,
            Scheme = seed.Scheme,
            PathBase = seed.PathBase,
            Path = seed.Path,
            QueryString = seed.QueryString,
            RawTarget = string.Concat(seed.PathBase, seed.Path, seed.QueryString),
            Headers = new HeaderDictionary(),
            Body = Stream.Null,
        };
        // The host name only: the cache key has no port.
        if (seed.Host.Length > 0)
            requestFeature.Headers["Host"] = seed.Host;
        foreach (var (name, value) in seed.Headers)
            requestFeature.Headers[name] = value;

        var features = new FeatureCollection();
        features.Set<IHttpRequestFeature>(requestFeature);
        features.Set<IHttpResponseFeature>(new CheckResponseFeature());
        features.Set<IHttpResponseBodyFeature>(new StreamResponseBodyFeature(body));
        // No local address or port: the cache key has neither.
        features.Set<IHttpConnectionFeature>(new HttpConnectionFeature
        {
            ConnectionId = context.Connection.Id + ":check",
            RemoteIpAddress = AgentSelfFetch.Label,
        });
        features.Set<IHttpRequestLifetimeFeature>(new HttpRequestLifetimeFeature
        {
            RequestAborted = context.RequestAborted,
        });

        var check = new DefaultHttpContext(features);
        if (seed.Endpoint != null)
        {
            check.SetEndpoint(seed.Endpoint);
            check.Request.RouteValues = new RouteValueDictionary(seed.RouteValues);
        }
        return check;
    }

    /// <summary>
    /// Runs <paramref name="next"/> for <paramref name="check"/> (from
    /// <see cref="Create"/>) and returns whether it answered with
    /// <paramref name="context"/>'s response status and media type. Call it
    /// after the response to <paramref name="context"/> is complete.
    /// </summary>
    internal static async Task<bool> RunAsync(
        HttpContext context, DefaultHttpContext check, RequestDelegate next,
        ILogger logger)
    {
        // Run without this request's ambient state, so that a service that
        // reads the current request (IHttpContextAccessor) reads the anonymous
        // one.
        Task<bool> run;
        using (ExecutionContext.SuppressFlow())
        {
            run = Task.Run(() => RunCoreAsync(context, check, next, logger));
        }
        return await run;
    }

    /// <summary>
    /// <see cref="Create"/> and <see cref="RunAsync"/> with the body discarded,
    /// for <paramref name="seed"/> or, when null, <paramref name="context"/>'s
    /// request as it is now.
    /// </summary>
    internal static Task<bool> IsGivenAsync(
        HttpContext context, RequestDelegate next, ILogger logger, Seed? seed = null) =>
        RunAsync(context, Create(context, seed ?? Seed.Capture(context), Stream.Null),
                 next, logger);

    private static async Task<bool> RunCoreAsync(
        HttpContext context, DefaultHttpContext check, RequestDelegate next,
        ILogger logger)
    {
        var responseFeature = (CheckResponseFeature)check.Features.Get<IHttpResponseFeature>()!;
        AsyncServiceScope? scope = null;
        IHttpContextAccessor? accessor = null;
        try
        {
            context.RequestAborted.ThrowIfCancellationRequested();
            var services = context.RequestServices;
            var scopeFactory = services?.GetService<IServiceScopeFactory>();
            if (scopeFactory != null)
            {
                scope = scopeFactory.CreateAsyncScope();
                check.RequestServices = scope.Value.ServiceProvider;
            }
            accessor = services?.GetService<IHttpContextAccessor>();
            if (accessor != null) accessor.HttpContext = check;

            await next(check);
            await responseFeature.RunStartingAsync();
            context.RequestAborted.ThrowIfCancellationRequested();

            return check.Response.StatusCode == context.Response.StatusCode &&
                   SameMediaType(check.Response.ContentType, context.Response.ContentType);
        }
        catch (Exception ex)
        {
            logger.LogDebug(ex,
                "Anonymous request for {Path} failed; not storing the response",
                context.Request.Path.Value);
            return false;
        }
        finally
        {
            try { await responseFeature.RunCompletedAsync(); }
            catch (Exception ex)
            {
                logger.LogDebug(ex, "Anonymous request completion callback failed");
            }
            if (accessor != null) accessor.HttpContext = null;
            if (scope is { } s) await s.DisposeAsync();
        }
    }

    /// <summary>
    /// Whether two Content-Type values name the same media type (parameters
    /// such as charset ignored, case-insensitive). Two absent values match.
    /// </summary>
    internal static bool SameMediaType(string? a, string? b) =>
        string.Equals(MediaType(a), MediaType(b), StringComparison.OrdinalIgnoreCase);

    private static string MediaType(string? contentType)
    {
        if (string.IsNullOrEmpty(contentType)) return string.Empty;
        var semicolon = contentType.IndexOf(';');
        return (semicolon < 0 ? contentType : contentType[..semicolon]).Trim();
    }

    /// <summary>
    /// A response feature that keeps the starting and completed callbacks
    /// the application registers, so they run as they would for a real
    /// response.
    /// </summary>
    private sealed class CheckResponseFeature : HttpResponseFeature
    {
        private readonly List<(Func<object, Task> Callback, object State)> _starting = new();
        private readonly List<(Func<object, Task> Callback, object State)> _completed = new();

        public override void OnStarting(Func<object, Task> callback, object state) =>
            _starting.Add((callback, state));

        public override void OnCompleted(Func<object, Task> callback, object state) =>
            _completed.Add((callback, state));

        // Last registered first, as the server runs them.
        public async Task RunStartingAsync()
        {
            for (var i = _starting.Count - 1; i >= 0; i--)
                await _starting[i].Callback(_starting[i].State);
            _starting.Clear();
        }

        public async Task RunCompletedAsync()
        {
            for (var i = _completed.Count - 1; i >= 0; i--)
                await _completed[i].Callback(_completed[i].State);
            _completed.Clear();
        }
    }
}

/// <summary>
/// URLs whose anonymous request was not given the visitor's response,
/// remembered for a short time so that a restricted URL does not make the
/// application answer twice on every request. Bounded: at the size cap,
/// expired entries are dropped, and if none expired the memo starts over.
/// Same idea as the worker's verdict memo (src/worker/subresource_gate.h).
/// </summary>
internal sealed class RefusalMemo
{
    /// <summary>How long a refusal is remembered.</summary>
    public static readonly TimeSpan DefaultTtl = TimeSpan.FromSeconds(60);

    /// <summary>The most URLs remembered at once.</summary>
    public const int DefaultMaxEntries = 8192;

    private readonly ConcurrentDictionary<string, long> _until = new(StringComparer.Ordinal);
    private readonly long _ttlMs;
    private readonly int _maxEntries;
    private readonly Func<long> _nowMs;

    public RefusalMemo()
        : this(DefaultTtl, DefaultMaxEntries, () => Environment.TickCount64) { }

    public RefusalMemo(TimeSpan ttl, int maxEntries, Func<long> nowMs)
    {
        _ttlMs = (long)ttl.TotalMilliseconds;
        _maxEntries = Math.Max(1, maxEntries);
        _nowMs = nowMs;
    }

    /// <summary>The number of URLs remembered now.</summary>
    public int Count => _until.Count;

    /// <summary>The memo key: the cache key's parts.</summary>
    public static string Key(string scheme, string hostname, string url) =>
        string.Concat(scheme, "\n", hostname, "\n", url);

    /// <summary>Whether <paramref name="key"/> was refused within the TTL.</summary>
    public bool IsRefused(string key)
    {
        if (!_until.TryGetValue(key, out var until)) return false;
        if (_nowMs() < until) return true;
        _until.TryRemove(new KeyValuePair<string, long>(key, until));
        return false;
    }

    /// <summary>Remembers <paramref name="key"/> as refused.</summary>
    public void Refuse(string key)
    {
        if (_until.Count >= _maxEntries && !_until.ContainsKey(key))
        {
            var now = _nowMs();
            foreach (var entry in _until)
            {
                if (entry.Value <= now) _until.TryRemove(entry);
            }
            if (_until.Count >= _maxEntries) _until.Clear();
        }
        _until[key] = _nowMs() + _ttlMs;
    }
}
