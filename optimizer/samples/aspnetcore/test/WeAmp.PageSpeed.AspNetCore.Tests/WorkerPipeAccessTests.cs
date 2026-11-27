// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.IO.Pipes;
using System.Runtime.Versioning;
using System.Security.Principal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The worker grants other accounts 0x12019b on its pipes and withholds the
/// pipe-instance right. A pipe whose only entry grants the current user that
/// mask is reached by this process the way a foreign account is.
/// </summary>
public class WorkerPipeAccessTests
{
    private const PipeAccessRights WorkerClientMask = (PipeAccessRights)0x12019b;

    [Fact]
    [SupportedOSPlatform("windows")]
    public async Task NotificationClient_ConnectsToAPipeThatGrantsOnlyTheClientMask()
    {
        if (!OperatingSystem.IsWindows())
            return; // Windows pipe access rules only.

        var name = "pagespeed-client-mask-" + Guid.NewGuid().ToString("N");
        var security = new PipeSecurity();
        security.AddAccessRule(new PipeAccessRule(
            WindowsIdentity.GetCurrent().User!, WorkerClientMask,
            System.Security.AccessControl.AccessControlType.Allow));

        using var server = NamedPipeServerStreamAcl.Create(
            name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous, 4096, 4096, security);

        // The stock client asks for GENERIC_WRITE and is denied.
        using (var stock = new NamedPipeClientStream(
                   ".", name, PipeDirection.Out, PipeOptions.Asynchronous,
                   System.Security.Principal.TokenImpersonationLevel.Identification))
        {
            await Assert.ThrowsAsync<UnauthorizedAccessException>(
                () => stock.ConnectAsync(2000));
        }

        // The service's client asks only for write data and connects (on every target
        // framework: net8.0 opens the pipe directly, net9.0 and later use the
        // access-rights constructor).
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var client = await WorkerNotificationService.ConnectPipeClientAsync(
            name, cts.Token);
        Assert.True(client.IsConnected);
    }
}
