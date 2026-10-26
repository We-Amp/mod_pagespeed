// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using WeAmp.PageSpeed.AspNetCore.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

public class TestTimeoutsTests
{
    private static Func<string, string?> Env(params (string Key, string Value)[] vars) =>
        key => vars.Where(v => v.Key == key).Select(v => v.Value).FirstOrDefault();

    [Fact]
    public void EnvOverride_Wins_OverCiValue()
    {
        var env = Env(("GITHUB_ACTIONS", "true"), (TestTimeouts.StartupEnvVar, "240"));

        TestTimeouts.Resolve(env, TestTimeouts.StartupEnvVar, TestTimeouts.CiStartupMs, 30_000)
            .Should().Be(240_000);
    }

    [Fact]
    public void CiValue_UnderGitHubActions_WithoutOverride()
    {
        var env = Env(("GITHUB_ACTIONS", "true"));

        TestTimeouts.Resolve(env, TestTimeouts.StartupEnvVar, TestTimeouts.CiStartupMs, 30_000)
            .Should().Be(180_000);
        TestTimeouts.Resolve(env, TestTimeouts.ShutdownEnvVar, TestTimeouts.CiShutdownMs, 10_000)
            .Should().Be(60_000);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("false")]
    public void ProductionDefault_Otherwise(string? githubActions)
    {
        var env = githubActions is null ? Env() : Env(("GITHUB_ACTIONS", githubActions));
        var defaults = new SidecarOptions();

        TestTimeouts.Resolve(env, TestTimeouts.StartupEnvVar, TestTimeouts.CiStartupMs, defaults.StartupTimeoutMs)
            .Should().Be(defaults.StartupTimeoutMs);
    }

    [Fact]
    public void InvalidOverride_IsIgnored()
    {
        var env = Env(("GITHUB_ACTIONS", "true"), (TestTimeouts.StartupEnvVar, "abc"));

        TestTimeouts.Resolve(env, TestTimeouts.StartupEnvVar, TestTimeouts.CiStartupMs, 30_000)
            .Should().Be(180_000);
    }
}
