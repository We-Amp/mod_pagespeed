// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Text.RegularExpressions;
using FluentAssertions;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The sidecar's private directory and generated conf are owner-only from creation,
/// and the directory name keeps the full GUID.
/// </summary>
public class SidecarPrivateStateTests
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
    public void OwnedConfigDirectory_UsesTheFullGuid()
    {
        var manager = NewManager();
        var dir = manager.ResolveConfigDirectory(new SidecarOptions());
        try
        {
            Regex.IsMatch(Path.GetFileName(dir), @"^ps-sidecar-\d+-[0-9a-f]{32}$")
                .Should().BeTrue("the random part must keep all 122 random bits of the GUID");
        }
        finally { TryDeleteDir(dir); }
    }

    [Fact]
    public void OwnedConfigDirectory_IsOwnerOnly()
    {
        if (OperatingSystem.IsWindows()) return; // POSIX modes do not exist on Windows; the profile ACL applies.

        var manager = NewManager();
        var dir = manager.ResolveConfigDirectory(new SidecarOptions());
        try
        {
            File.GetUnixFileMode(dir).Should().Be(
                UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        }
        finally { TryDeleteDir(dir); }
    }

    [Fact]
    public void WriteOwnerOnlyFile_IsOwnerOnlyAndWritesContents()
    {
        var dir = Path.Combine(Path.GetTempPath(), "ps-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var path = Path.Combine(dir, "nginx.conf");

            NginxConfigGenerator.WriteOwnerOnlyFile(path, "# conf\n");

            File.ReadAllText(path).Should().Be("# conf\n");
            if (!OperatingSystem.IsWindows())
            {
                File.GetUnixFileMode(path).Should().Be(UnixFileMode.UserRead | UnixFileMode.UserWrite);
            }
        }
        finally { TryDeleteDir(dir); }
    }

    [Fact]
    public void WriteOwnerOnlyFile_ReplacingWiderModeFile_ResultIsOwnerOnly()
    {
        if (OperatingSystem.IsWindows()) return; // POSIX modes do not exist on Windows.

        var dir = Path.Combine(Path.GetTempPath(), "ps-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var path = Path.Combine(dir, "nginx.conf");
            File.WriteAllText(path, "old");
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite
                | UnixFileMode.GroupRead | UnixFileMode.OtherRead);

            NginxConfigGenerator.WriteOwnerOnlyFile(path, "new");

            File.ReadAllText(path).Should().Be("new");
            File.GetUnixFileMode(path).Should().Be(UnixFileMode.UserRead | UnixFileMode.UserWrite);
            Directory.GetFiles(dir).Should().HaveCount(1, "no temp file is left behind");
        }
        finally { TryDeleteDir(dir); }
    }

    private static void TryDeleteDir(string dir)
    {
        try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
    }

    private sealed class StubHttpClientFactory : System.Net.Http.IHttpClientFactory
    {
        public System.Net.Http.HttpClient CreateClient(string name) => new();
    }
}
