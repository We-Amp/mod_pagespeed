using FluentAssertions;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 1 — PageSpeedOptionsValidator Inverse-mode rules. NOTE: the validator
/// hard-rejects Inverse on non-Linux, so the "valid Inverse" baseline used by the
/// negative tests is only itself valid on Linux; each test below targets a SINGLE
/// failure and asserts on its specific message so it is deterministic on macOS too
/// (where the non-Linux reject is ALSO present but the asserted message still
/// appears in the aggregate failure).
/// </summary>
public class InverseValidatorTests
{
    private static ValidateOptionsResult Validate(PageSpeedOptions options) =>
        new PageSpeedOptionsValidator().Validate(null, options);

    private static PageSpeedOptions Inverse(Action<SidecarOptions>? sidecar = null, Action<PageSpeedOptions>? root = null)
    {
        var o = new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse, OwnPublicPort = true } };
        sidecar?.Invoke(o.Sidecar);
        root?.Invoke(o);
        return o;
    }

    [Theory]
    [InlineData(8080, 8080, 5000)] // ListenPort == NginxLoopbackPort
    [InlineData(8080, 5100, 8080)] // ListenPort == OriginPort
    [InlineData(8080, 5100, 5100)] // NginxLoopbackPort == OriginPort
    public void ThreeWayPortDistinctness_RejectedOnCollision(int listen, int nginx, int origin)
    {
        var r = Validate(Inverse(s =>
        {
            s.ListenPort = listen;
            s.NginxLoopbackPort = nginx;
            s.OriginPort = origin;
        }));
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("differ");
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(70000)]
    public void NginxLoopbackPort_RangeRejected(int port)
    {
        var r = Validate(Inverse(s => s.NginxLoopbackPort = port));
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("NginxLoopbackPort");
    }

    [Fact]
    public void Inverse_WithAdminAuthDisabled_IsRejected()
    {
        var r = Validate(Inverse(root: o => o.AdminAuth.Enabled = false));
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("AdminAuth:Enabled must be true",
            "the module IP-ACL is nullified in Inverse, so the bearer is load-bearing");
    }

    [Fact]
    public void Inverse_WithUnixSocketRawOrigin_IsRejected()
    {
        var r = Validate(Inverse(s => s.SocketPath = "/tmp/ps/origin.sock"));
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("SocketPath",
            "the loop break needs an authoritative loopback-TCP LocalPort; a UDS raw origin is rejected");
    }

    [Fact]
    public void Inverse_OnNonLinux_IsRejected()
    {
        if (OperatingSystem.IsLinux())
        {
            // On Linux this rule does not fire; nothing to assert here.
            return;
        }
        var r = Validate(Inverse());
        r.Failed.Should().BeTrue();
        r.FailureMessage.Should().Contain("Linux-only");
    }

    [Fact]
    public void Inverse_WithNoPublicEndpoint_Warns()
    {
        // OwnPublicPort=false (the edge-TLS default) emits a non-fatal warning via
        // the injected logger; it must NOT add a validation failure for that reason.
        var capturing = new ListLogger<PageSpeedOptionsValidator>();
        var validator = new PageSpeedOptionsValidator(capturing);
        var opts = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse, OwnPublicPort = false }
        };

        validator.Validate(null, opts);

        capturing.Messages.Should().Contain(m => m.Contains("OwnPublicPort"),
            "Inverse with no auto-bound public endpoint should warn (non-fatal)");
    }

    [Fact]
    public void Process_And_External_Validation_Unchanged()
    {
        // Regression: Process is valid on every OS (no Linux-only reject), and the
        // existing OriginPort==ListenPort collision still fails for Process.
        Validate(new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Process } })
            .Succeeded.Should().BeTrue();
        Validate(new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.External } })
            .Succeeded.Should().BeTrue();

        var collision = Validate(new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Process, ListenPort = 8080, OriginPort = 8080 }
        });
        collision.Failed.Should().BeTrue();
        collision.FailureMessage.Should().Contain("differ");
    }

    [Fact]
    public void Valid_Inverse_OnLinux_Succeeds()
    {
        if (!OperatingSystem.IsLinux())
        {
            return; // the Linux-only reject would fire off-Linux; covered elsewhere.
        }
        // Distinct ports, AdminAuth on, no UDS, OwnPublicPort on.
        var r = Validate(Inverse(s =>
        {
            s.ListenPort = 8080;
            s.NginxLoopbackPort = 5100;
            s.OriginPort = 5000;
        }));
        r.Succeeded.Should().BeTrue();
    }

    [Fact]
    public void Inverse_DefaultForwardAll_ImposesNoHostObligation()
    {
        // The default path (RestrictToAuthorizedHosts = false, default AuthorizedDomains)
        // must NOT warn about hosts — forward-all imposes zero operator obligation.
        var capturing = new ListLogger<PageSpeedOptionsValidator>();
        var validator = new PageSpeedOptionsValidator(capturing);
        var opts = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse, OwnPublicPort = true }
        };

        validator.Validate(null, opts);

        capturing.Messages.Should().NotContain(m => m.Contains("RestrictToAuthorizedHosts"),
            "forward-all is the default and requires no host configuration");
    }

    [Fact]
    public void Inverse_RestrictWithOnlyLoopbackSeed_Warns_NonFatal()
    {
        // Opting into the strict allowlist but leaving only the loopback seed would
        // serve every public host un-optimized → a non-fatal warning, NOT an error.
        var capturing = new ListLogger<PageSpeedOptionsValidator>();
        var validator = new PageSpeedOptionsValidator(capturing);
        var opts = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions
            {
                Mode = SidecarMode.Inverse,
                OwnPublicPort = true,
                RestrictToAuthorizedHosts = true,
            }
            // Domains.AuthorizedDomains keeps its default [localhost, 127.0.0.1] seed.
        };

        var r = validator.Validate(null, opts);

        capturing.Messages.Should().Contain(m => m.Contains("RestrictToAuthorizedHosts"),
            "strict mode with only the loopback seed should warn");
        if (OperatingSystem.IsLinux())
        {
            r.Succeeded.Should().BeTrue("the host-seed warning is non-fatal");
        }
    }

    private sealed class ListLogger<T> : ILogger<T>
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
