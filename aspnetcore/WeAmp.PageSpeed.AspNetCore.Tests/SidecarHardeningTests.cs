// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Runtime.InteropServices;
using FluentAssertions;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Regression tests for the design record review hardening pass: artifact/secret cleanup on
/// Dispose (SEC-1), options validation incl. unsupported Docker mode (DI-1/DI-2),
/// module-path-follows-binary resolution (PKG-3), the nginx -t config-test invocation
/// (SPEC-1), and the unmapped-option warnings (SPEC-2).
/// </summary>
public class SidecarHardeningTests
{
    // ---------------- SEC-1: artifact / secret cleanup ----------------

    [Fact]
    public void CleanupArtifacts_WhenOwned_RemovesTheWholeDirIncludingOtherFiles()
    {
        var dir = NewTempDir();
        var conf = Path.Combine(dir, "nginx.conf");
        var other = Path.Combine(dir, "operator.secret");
        File.WriteAllText(conf, "# generated");
        File.WriteAllText(other, "secret-value");

        ProcessSidecarManager.CleanupArtifacts(dir, ownsConfigDir: true, configPath: conf);

        Directory.Exists(dir).Should().BeFalse("a sidecar-owned temp dir (the bearer-token conf and anything beside it) must be reaped");
    }

    [Fact]
    public void CleanupArtifacts_WhenOperatorPinned_RemovesOnlyTheGeneratedConf_KeepsOtherFiles()
    {
        var dir = NewTempDir();
        var conf = Path.Combine(dir, "nginx.conf");
        var other = Path.Combine(dir, "operator.secret");
        File.WriteAllText(conf, "# generated");
        File.WriteAllText(other, "secret-value");
        try
        {
            ProcessSidecarManager.CleanupArtifacts(dir, ownsConfigDir: false, configPath: conf);

            Directory.Exists(dir).Should().BeTrue("an operator-pinned ConfigDirectory must be left intact");
            File.Exists(conf).Should().BeFalse("the generated nginx.conf is still removed");
            File.Exists(other).Should().BeTrue(
                "files the operator keeps in a pinned dir (cache, their own secrets) are theirs to keep");
        }
        finally { TryDeleteDir(dir); }
    }

    [Fact]
    public void CleanupArtifacts_MissingDir_IsSafeNoOp()
    {
        var ghost = Path.Combine(Path.GetTempPath(), "psh_" + Guid.NewGuid().ToString("N"));
        Action act = () => ProcessSidecarManager.CleanupArtifacts(ghost, ownsConfigDir: true, configPath: null);
        act.Should().NotThrow();
    }

    // ---------------- DI-1: Docker mode fails clearly, never spawns ----------------

    [Fact]
    public async Task StartAsync_DockerMode_ThrowsNotSupported_WithoutSpawning()
    {
        var options = Microsoft.Extensions.Options.Options.Create(new PageSpeedOptions
        {
            Enabled = true,
            Sidecar = new SidecarOptions { Mode = SidecarMode.Docker }
        });
        var manager = new ProcessSidecarManager(
            NullLogger<ProcessSidecarManager>.Instance, options, NewGenerator(), new StubHttpClientFactory(),
            new InternalSidecarEndpoint());

        await manager.Invoking(m => m.StartAsync(CancellationToken.None))
            .Should().ThrowAsync<NotSupportedException>()
            .WithMessage("*Docker*");
    }

    // ---------------- DI-2: options validator ----------------

    [Fact]
    public void Validator_DefaultOptions_Succeed()
    {
        // The bare default is now SidecarMode.Inverse (Linux-only); pin to Process
        // so this regression check runs on every CI dim (incl. non-Linux dev boxes).
        // The Inverse-specific validator rules are covered by InverseValidatorTests.
        Validate(new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Process } })
            .Succeeded.Should().BeTrue();
    }

    [Fact]
    public void Validator_DockerMode_Fails()
    {
        var r = Validate(new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Docker } });
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("Docker");
    }

    [Fact]
    public void Validator_OriginPortEqualsListenPort_Fails()
    {
        var r = Validate(new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process, ListenPort = 8080, OriginPort = 8080 }
        });
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("differ");
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(70000)]
    public void Validator_OutOfRangeOriginPort_Fails(int originPort)
    {
        // Pin to Process: the OriginPort RANGE check is mode-independent, but the
        // bare default is now Linux-only Inverse which would add unrelated failures
        // on a non-Linux dev box.
        var r = Validate(new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process, OriginPort = originPort }
        });
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("OriginPort");
    }

    [Fact]
    public void Validator_ExternalMode_Succeeds()
    {
        Validate(new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.External } })
            .Succeeded.Should().BeTrue();
    }

    // ---------------- PKG-3: module path follows the resolved binary ----------------

    [Fact]
    public void ResolveModulePath_DerivesFromResolvedBinaryDir()
    {
        var binary = Path.Combine("/opt", "bundle", "runtimes", "linux-x64", "native", "nginx");
        var expected = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(binary))!, "ngx_pagespeed_module.so");

        NginxConfigGenerator.ResolveModulePath(new PageSpeedOptions(), binary).Should().Be(expected);
    }

    [Fact]
    public void ResolveModulePath_ResolvedBinaryWinsOverConfiguredBinaryPath()
    {
        var configuredBinary = Path.Combine("/elsewhere", "nginx");
        var resolvedBinary = Path.Combine("/opt", "bundle", "nginx");
        var opts = new PageSpeedOptions { Sidecar = new SidecarOptions { BinaryPath = configuredBinary } };
        var expected = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(resolvedBinary))!, "ngx_pagespeed_module.so");

        NginxConfigGenerator.ResolveModulePath(opts, resolvedBinary).Should().Be(expected);
    }

    [Fact]
    public void ResolveModulePath_ExplicitModulePath_Wins()
    {
        var module = Path.Combine("/custom", "mymod.so");
        var opts = new PageSpeedOptions { Sidecar = new SidecarOptions { ModulePath = module } };

        NginxConfigGenerator.ResolveModulePath(opts, Path.Combine("/opt", "bundle", "nginx"))
            .Should().Be(Path.GetFullPath(module));
    }

    [Fact]
    public void GenerateConfig_LoadModule_FollowsResolvedBinaryDir()
    {
        var binary = Path.Combine("/opt", "bundle", "runtimes", "linux-x64", "native", "nginx");
        var expectedModule = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(binary))!, "ngx_pagespeed_module.so");
        var prefix = NewTempDir();
        try
        {
            var (conf, _) = NewGenerator().GenerateConfig(
                new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Process } }, prefix, binary);
            conf.Should().Contain($"load_module {expectedModule};");
        }
        finally { TryDeleteDir(prefix); }
    }

    // ---------------- SPEC-1: nginx -t config-test invocation ----------------

    [Fact]
    public void BuildConfigTestStartInfo_UsesArgumentList_WithDashT()
    {
        var psi = ProcessSidecarManager.BuildConfigTestStartInfo("/opt/ps/nginx", "/opt/ps/run/nginx.conf");
        var prefix = Path.GetDirectoryName(Path.GetFullPath("/opt/ps/run/nginx.conf"))!;

        psi.Arguments.Should().BeNullOrEmpty("the unescaped Arguments string must not be used");
        psi.ArgumentList.Should().ContainInOrder("-p", prefix, "-c", "/opt/ps/run/nginx.conf", "-t");
        psi.RedirectStandardError.Should().BeTrue("nginx -t diagnostics must be captured");
    }

    // ---------------- Inverse: health poll dials the nginx loopback port ----------------

    [Fact]
    public void ResolveHealthPort_Process_DialsListenPort()
    {
        var opts = new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Process, ListenPort = 8080 } };
        var ep = new InternalSidecarEndpoint();
        ProcessSidecarManager.ResolveHealthPort(opts, ep).Should().Be(8080,
            "in Process mode nginx is the public front-end on ListenPort");
    }

    [Fact]
    public void ResolveHealthPort_Inverse_DialsNginxLoopbackPort_NotListenPort()
    {
        var opts = new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse, ListenPort = 8080 } };
        var ep = new InternalSidecarEndpoint();
        ep.SetPort(5000);
        ep.SetRawOriginLoopbackPort(5000);
        ep.SetNginxLoopbackPort(5100);

        ProcessSidecarManager.ResolveHealthPort(opts, ep).Should().Be(5100,
            "in Inverse nginx listens loopback-only on NginxLoopbackPort; ListenPort is now the public Kestrel port");
    }

    // ---------------- UX-6: bundled RID follows the running architecture ----------------

    [Theory]
    [InlineData(Architecture.X64, "linux-x64")]
    [InlineData(Architecture.Arm64, "linux-arm64")]
    public void ResolveBundledRid_MapsArchToNuGetRid(Architecture arch, string expected)
    {
        ProcessSidecarManager.ResolveBundledRid(arch).Should().Be(expected,
            "the matched nginx is published under runtimes/<rid>/native/ for both arches (UX-6)");
    }

    // ---------------- UX-7: PR_SET_PDEATHSIG launch shim ----------------

    [Fact]
    public void BuildStartInfo_NoShim_LaunchesNginxDirectly()
    {
        var dir = NewTempDir();
        try
        {
            var nginx = Path.Combine(dir, "nginx");
            File.WriteAllText(nginx, "x"); // no pagespeed-nginx-launch alongside it
            var conf = Path.Combine(dir, "nginx.conf");
            var prefix = Path.GetDirectoryName(Path.GetFullPath(conf))!;

            var psi = ProcessSidecarManager.BuildStartInfo(nginx, conf, new PageSpeedOptions());

            psi.FileName.Should().Be(nginx, "absent the shim, nginx is launched directly");
            psi.ArgumentList.Should().ContainInOrder("-p", prefix, "-c", conf, "-g", "daemon off;");
            psi.ArgumentList.Should().NotContain(nginx, "the binary path is FileName, not an argument, in the direct path");
        }
        finally { TryDeleteDir(dir); }
    }

    [Fact]
    public void BuildStartInfo_LinuxWithShim_ExecsNginxThroughTheShim()
    {
        // The shim sets PR_SET_PDEATHSIG, which is Linux-only; on other dev hosts the
        // production guard (OperatingSystem.IsLinux()) launches nginx directly, so this
        // assertion only applies on Linux.
        if (!OperatingSystem.IsLinux()) return;

        var dir = NewTempDir();
        try
        {
            var nginx = Path.Combine(dir, "nginx");
            var shim = Path.Combine(dir, "pagespeed-nginx-launch");
            File.WriteAllText(nginx, "x");
            File.WriteAllText(shim, "x");
            var conf = Path.Combine(dir, "nginx.conf");
            var prefix = Path.GetDirectoryName(Path.GetFullPath(conf))!;

            var psi = ProcessSidecarManager.BuildStartInfo(nginx, conf, new PageSpeedOptions());

            psi.FileName.Should().Be(shim, "the launch shim is preferred when bundled (UX-7)");
            psi.ArgumentList[0].Should().Be(nginx, "the shim execs argv[1..], so nginx is the first arg");
            psi.ArgumentList.Should().ContainInOrder(nginx, "-p", prefix, "-c", conf, "-g", "daemon off;");
        }
        finally { TryDeleteDir(dir); }
    }

    // ---------------- SPEC-2: unmapped-option warnings ----------------

    [Fact]
    public void GenerateConfig_WarnsWhenRedisConfigured()
    {
        var logger = new CapturingLogger<NginxConfigGenerator>();
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetPort(5000);
        var generator = new NginxConfigGenerator(logger, endpoint);
        var prefix = NewTempDir();
        try
        {
            generator.GenerateConfig(new PageSpeedOptions
            {
                Sidecar = new SidecarOptions { Mode = SidecarMode.Process },
                Redis = new RedisOptions { Host = "redis.local" }
            }, prefix);
            logger.Messages.Should().Contain(m => m.Contains("Redis"),
                "a configured-but-unmapped Redis surface must warn, not silently drop");
        }
        finally { TryDeleteDir(prefix); }
    }

    [Fact]
    public void GenerateConfig_WarnsWhenDomainMappingsConfigured()
    {
        var logger = new CapturingLogger<NginxConfigGenerator>();
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetPort(5000);
        var generator = new NginxConfigGenerator(logger, endpoint);
        var prefix = NewTempDir();
        try
        {
            var opts = new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Process } };
            opts.Domains.Shards.Add(new DomainShard { Domain = "example.com", Shards = "s1.example.com,s2.example.com" });
            generator.GenerateConfig(opts, prefix);
            logger.Messages.Should().Contain(m => m.Contains("rewrite/origin/shard"));
        }
        finally { TryDeleteDir(prefix); }
    }

    // ------------------------------- helpers -------------------------------

    private static NginxConfigGenerator NewGenerator(int originPort = 5000)
    {
        var endpoint = new InternalSidecarEndpoint();
        endpoint.SetPort(originPort);
        return new NginxConfigGenerator(NullLogger<NginxConfigGenerator>.Instance, endpoint);
    }

    private static ValidateOptionsResult Validate(PageSpeedOptions options) =>
        new PageSpeedOptionsValidator().Validate(null, options);

    private static string NewTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "psh_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static void TryDeleteDir(string dir)
    {
        try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
    }

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
