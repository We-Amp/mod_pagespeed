// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Reflection;
using FluentAssertions;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Waiting for the sidecar to become healthy reports a startup timeout as a
/// TimeoutException and a caller cancellation as an OperationCanceledException.
/// </summary>
public class SidecarStartupTimeoutTests
{
    private static Task WaitForHealthy(int timeoutMs, CancellationToken token)
    {
        var endpoint = new InternalSidecarEndpoint();
        var manager = new ProcessSidecarManager(
            NullLogger<ProcessSidecarManager>.Instance,
            Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions()),
            new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint),
            new StubHttpClientFactory(),
            endpoint);
        var method = typeof(ProcessSidecarManager).GetMethod(
            "WaitForHealthyAsync", BindingFlags.Instance | BindingFlags.NonPublic)!;
        return (Task)method.Invoke(manager, new object[] { timeoutMs, token })!;
    }

    [Fact]
    public async Task StartupTimeout_ThrowsTimeoutException()
    {
        var act = () => WaitForHealthy(300, CancellationToken.None);
        await act.Should().ThrowAsync<TimeoutException>();
    }

    [Fact]
    public async Task CallerCancellation_StillThrowsOperationCanceled()
    {
        using var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(100));
        var act = () => WaitForHealthy(60_000, cts.Token);
        await act.Should().ThrowAsync<OperationCanceledException>();
    }

    private sealed class StubHttpClientFactory : System.Net.Http.IHttpClientFactory
    {
        public System.Net.Http.HttpClient CreateClient(string name) => new();
    }
}
