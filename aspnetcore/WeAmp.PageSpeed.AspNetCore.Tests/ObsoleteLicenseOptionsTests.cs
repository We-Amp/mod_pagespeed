// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// mod_pagespeed 2.1 removed licensing from the module and from this package. The
/// <see cref="PageSpeedOptions.LicenseKey"/> and
/// <see cref="PageSpeedOptions.LicenseServiceUrl"/> properties survive only as
/// [Obsolete] no-ops so existing configuration and binder code keep compiling and
/// binding. These tests pin the no-op: setting them changes nothing the sidecar
/// hands to the nginx child, writes no file, logs nothing, and the package adds no
/// environment of its own.
/// </summary>
public class ObsoleteLicenseOptionsTests
{
    // Exercising the obsolete members IS the point of these tests.
#pragma warning disable CS0618

    [Fact]
    public void BuildChildEnvironment_ObsoleteLicenseOptions_AddNothing()
    {
        var opts = new PageSpeedOptions
        {
            LicenseKey = "some-configured-key",
            LicenseServiceUrl = "https://lic.example.com/api",
        };
        opts.Sidecar.EnvironmentVariables["PS_TEST_OPERATOR_VAR"] = "operator-value";

        var env = ProcessSidecarManager.BuildChildEnvironment(opts);

        env.Should().HaveCount(1,
            "the child environment is exactly the operator's escape-hatch entries — the package injects nothing");
        env.Should().ContainKey("PS_TEST_OPERATOR_VAR").WhoseValue.Should().Be("operator-value");
        env.Keys.Should().NotContain(k => k.Contains("LICENSE", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void BuildChildEnvironment_NothingConfigured_IsEmpty()
    {
        ProcessSidecarManager.BuildChildEnvironment(new PageSpeedOptions()).Should().BeEmpty(
            "with nothing configured the package hands the child no environment of its own");
    }

    [Fact]
    public void BuildChildEnvironment_EnvironmentVariables_ArePassedThrough()
    {
        var opts = new PageSpeedOptions();
        opts.Sidecar.EnvironmentVariables["FOO"] = "bar";
        ProcessSidecarManager.BuildChildEnvironment(opts).Should().ContainKey("FOO").WhoseValue.Should().Be("bar");
    }

    [Fact]
    public void BuildStartInfo_ObsoleteLicenseOptions_OnlyOperatorEntriesReachTheChild()
    {
        var opts = new PageSpeedOptions
        {
            LicenseKey = "some-configured-key",
            LicenseServiceUrl = "https://lic.example.com/api",
        };
        opts.Sidecar.EnvironmentVariables["PS_TEST_OPERATOR_VAR"] = "operator-value";

        var psi = ProcessSidecarManager.BuildStartInfo("/opt/ps/nginx", "/opt/ps/run/nginx.conf", opts);

        // ProcessStartInfo.Environment starts as a copy of THIS process's environment;
        // whatever the sidecar layers on top must be exactly the operator's entries.
        var inherited = Environment.GetEnvironmentVariables().Keys.Cast<string>().ToHashSet(StringComparer.Ordinal);
        var added = psi.Environment.Keys.Where(k => !inherited.Contains(k)).ToList();

        added.Should().BeEquivalentTo(new[] { "PS_TEST_OPERATOR_VAR" },
            "no license-related (or any other) variable is added to the nginx child");
        psi.Environment["PS_TEST_OPERATOR_VAR"].Should().Be("operator-value");
    }

    [Fact]
    public async Task StartAsync_ObsoleteLicenseOptions_WritesNoLicenseFile_LogsNothingAboutLicensing()
    {
        // Drives the real pre-launch path (private config dir, generated config, nginx -t)
        // with /bin/false standing in for nginx, so the config test fails fast and nothing
        // is ever launched. Before 2.1 this path wrote parent(FileCachePath)/pagespeed.license
        // and logged about the token; both must be gone.
        if (!OperatingSystem.IsLinux()) return;

        var dir = Path.Combine(Path.GetTempPath(), "psobs_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var options = Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions
            {
                Enabled = true,
                LicenseKey = "some-configured-key",
                LicenseServiceUrl = "https://lic.example.com/api",
                Sidecar = new SidecarOptions
                {
                    Mode = SidecarMode.Process,
                    BinaryPath = "/bin/false",
                    ConfigDirectory = dir,
                },
                Cache = new CacheOptions { FileCachePath = Path.Combine(dir, "cache") },
            });
            var logger = new CapturingLogger<ProcessSidecarManager>();
            var endpoint = new InternalSidecarEndpoint();
            endpoint.SetPort(5000);
            using var manager = new ProcessSidecarManager(
                logger, options, new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint),
                new StubHttpClientFactory(), endpoint);

            await manager.Invoking(m => m.StartAsync(CancellationToken.None))
                .Should().ThrowAsync<InvalidOperationException>("/bin/false fails the nginx -t config test");

            Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)
                .Select(Path.GetFileName)
                .Should().NotContain("pagespeed.license", "the sidecar no longer writes a license file");
            logger.Messages.Should().NotContain(m => m.Contains("license", StringComparison.OrdinalIgnoreCase),
                "nothing about licensing is logged, whatever the obsolete options hold");
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
        }
    }

#pragma warning restore CS0618

    // ------------------------------- helpers -------------------------------

    private sealed class StubHttpClientFactory : System.Net.Http.IHttpClientFactory
    {
        public System.Net.Http.HttpClient CreateClient(string name) => new();
    }

    private sealed class CapturingLogger<T> : ILogger<T>
    {
        public List<string> Messages { get; } = new();
        public IDisposable BeginScope<TState>(TState state) where TState : notnull => NullScope.Instance;
        public bool IsEnabled(LogLevel logLevel) => true;
        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
            => Messages.Add(formatter(state, exception));

        private sealed class NullScope : IDisposable
        {
            public static readonly NullScope Instance = new();
            public void Dispose() { }
        }
    }
}
