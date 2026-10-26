// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using WeAmp.PageSpeed.AspNetCore.Options;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Sidecar start and stop budgets for the live tests. The production defaults in
/// <see cref="SidecarOptions"/> are untouched; only the tests widen their budgets,
/// so a heavily loaded CI host does not fail them on slow process start.
/// Order: environment override, then the CI value under GitHub Actions, then the
/// production default.
/// </summary>
internal static class TestTimeouts
{
    internal const string StartupEnvVar = "PAGESPEED_TEST_SIDECAR_STARTUP_SECONDS";
    internal const string ShutdownEnvVar = "PAGESPEED_TEST_SIDECAR_SHUTDOWN_SECONDS";
    internal const int CiStartupMs = 180_000;
    internal const int CiShutdownMs = 60_000;

    internal static int StartupBudgetMs => Resolve(
        Environment.GetEnvironmentVariable, StartupEnvVar, CiStartupMs, new SidecarOptions().StartupTimeoutMs);

    internal static int ShutdownBudgetMs => Resolve(
        Environment.GetEnvironmentVariable, ShutdownEnvVar, CiShutdownMs, new SidecarOptions().ShutdownTimeoutMs);

    /// <summary>Applies both budgets to the sidecar options of a live-test host.</summary>
    internal static void Apply(SidecarOptions sidecar)
    {
        sidecar.StartupTimeoutMs = StartupBudgetMs;
        sidecar.ShutdownTimeoutMs = ShutdownBudgetMs;
    }

    internal static int Resolve(
        Func<string, string?> getEnv, string envVar, int ciMs, int defaultMs)
    {
        if (int.TryParse(getEnv(envVar), out var seconds) && seconds > 0)
        {
            return checked(seconds * 1000);
        }

        return string.Equals(getEnv("GITHUB_ACTIONS"), "true", StringComparison.OrdinalIgnoreCase)
            ? ciMs
            : defaultMs;
    }
}
