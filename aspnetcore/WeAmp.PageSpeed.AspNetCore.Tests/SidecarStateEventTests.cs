// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The StateChanged event is raised outside the manager's lock.
/// </summary>
public class SidecarStateEventTests
{
    private static ProcessSidecarManager NewManager()
    {
        var endpoint = new InternalSidecarEndpoint();
        return new ProcessSidecarManager(
            NullLogger<ProcessSidecarManager>.Instance,
            Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions()),
            new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint),
            new StubHttpClientFactory(),
            endpoint);
    }

    [Fact]
    public void StateChanged_SubscriberCallingBackIntoManager_DoesNotDeadlock()
    {
        var manager = NewManager();
        SidecarState? observedFromHandler = null;
        var calledBack = false;

        manager.StateChanged += (_, e) =>
        {
            observedFromHandler = manager.State;
            // A lock-taking member invoked from another thread while the handler
            // waits for it: this never completes if the event is raised under the lock.
            calledBack = Task.Run(() => manager.Dispose()).Wait(TimeSpan.FromSeconds(10));
            e.NewState.Should().Be(SidecarState.Running);
        };

        var raise = Task.Run(() => manager.SetState(SidecarState.Running));

        raise.Wait(TimeSpan.FromSeconds(20)).Should().BeTrue("the handler must be able to call back into the manager");
        calledBack.Should().BeTrue();
        observedFromHandler.Should().Be(SidecarState.Running, "the handler sees the state it was told about");
    }

    [Fact]
    public void StateChanged_NotRaisedForUnchangedState()
    {
        var manager = NewManager();
        var raised = 0;
        manager.StateChanged += (_, _) => raised++;

        manager.SetState(SidecarState.Running);
        manager.SetState(SidecarState.Running);

        raised.Should().Be(1);
    }

    private sealed class StubHttpClientFactory : System.Net.Http.IHttpClientFactory
    {
        public System.Net.Http.HttpClient CreateClient(string name) => new();
    }
}
