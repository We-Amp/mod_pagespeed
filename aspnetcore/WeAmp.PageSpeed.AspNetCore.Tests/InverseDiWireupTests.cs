// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.DependencyInjection;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using Yarp.ReverseProxy.Forwarder;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 1 — verifies the Inverse-mode DI graph resolves end-to-end (catches a
/// missing registration the predicate/transformer unit tests would bypass). No
/// nginx, no host start — just the container.
/// </summary>
public class InverseDiWireupTests
{
    // The forwarder registrations are mode-independent (registered in
    // AddPageSpeedCore for every mode); only UsePageSpeed gates whether the
    // middleware actually runs. Build the graph in PROCESS mode so options
    // validation (which rejects Inverse on non-Linux dev boxes) does not fire when
    // a factory reads IOptions.Value — the registrations are identical.
    private static ServiceProvider BuildProvider()
    {
        var services = new ServiceCollection();
        services.AddLogging();
        services.AddPageSpeed(o =>
        {
            o.Sidecar.Mode = SidecarMode.Process;
            o.AdminAuth.Enabled = true;
        });
        return services.BuildServiceProvider();
    }

    [Fact]
    public void Graph_ResolvesForwarderSeamAndTransformerAndInvoker()
    {
        using var sp = BuildProvider();

        sp.GetService<IHttpForwarder>().Should().NotBeNull("AddHttpForwarder() must be registered");
        sp.GetService<LoopbackHttpMessageInvoker>().Should().NotBeNull();
        sp.GetService<InverseForwardTransformer>().Should().NotBeNull();
        sp.GetService<IPageSpeedForwarder>().Should().BeOfType<HttpForwarderAdapter>();
        sp.GetService<InternalSidecarEndpoint>().Should().NotBeNull();
    }

    [Fact]
    public void RegistersBothKestrelConfigurators()
    {
        using var sp = BuildProvider();

        var configurators = sp.GetServices<IConfigureOptions<KestrelServerOptions>>().ToList();
        configurators.Should().Contain(c => c is SidecarKestrelConfigureOptions,
            "the raw-origin Kestrel configurator must be registered");
        configurators.Should().Contain(c => c is PublicKestrelConfigureOptions,
            "the Inverse public-bind configurator must be registered (separate from raw-origin)");
    }
}
