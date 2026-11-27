// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Whether a run must have the native assets (<c>libpagespeed</c> and
/// <c>factory_worker</c>). With <see cref="Variable"/> set to <c>1</c>, the
/// tests that need them run and FAIL when they are missing. Unset, those tests
/// are reported as skipped, never as passed.
/// </summary>
internal static class NativeAssetsRequirement
{
    public const string Variable = "PAGESPEED_E2E_REQUIRE_NATIVE";

    public static bool Required =>
        Environment.GetEnvironmentVariable(Variable) == "1";

    /// <summary>
    /// <see cref="Required"/>, or a CI run (the <c>CI</c> variable set to
    /// anything but empty, <c>0</c> or <c>false</c>). Used by
    /// <see cref="NativeLibTheoryAttribute"/>, so its tests cannot pass or
    /// skip without the library on a build machine.
    /// </summary>
    public static bool RequiredOrCi
    {
        get
        {
            if (Required) return true;
            var ci = Environment.GetEnvironmentVariable("CI");
            return !string.IsNullOrEmpty(ci) && ci != "0" &&
                   !ci.Equals("false", StringComparison.OrdinalIgnoreCase);
        }
    }

    public static string SkipReason(string why) =>
        $"{why} Set {Variable}=1 to fail instead of skipping.";
}

/// <summary>
/// A fact that runs the auto-started worker through
/// <see cref="NativeAssetsFixture"/>: skipped when the worker binary or the
/// native library is not available (see
/// <see cref="NativeAssetsFixture.WhyNotRunnable"/>), unless
/// <see cref="NativeAssetsRequirement.Variable"/> requires them.
/// </summary>
public sealed class WorkerFactAttribute : FactAttribute
{
    public WorkerFactAttribute()
    {
        if (NativeAssetsRequirement.Required) return;
        var why = NativeAssetsFixture.WhyNotRunnable();
        if (why != null) Skip = NativeAssetsRequirement.SkipReason(why);
    }
}

/// <summary>
/// A fact that needs only the native library (not the worker): skipped when
/// the library is not loadable in this run, unless
/// <see cref="NativeAssetsRequirement.Variable"/> requires it, in which case
/// the test runs and fails.
/// </summary>
public sealed class NativeLibFactAttribute : FactAttribute
{
    public NativeLibFactAttribute()
    {
        if (NativeAssetsRequirement.Required) return;
        if (!NativeLibAbsentFactAttribute.IsLibraryLoadable())
            Skip = NativeAssetsRequirement.SkipReason(
                "The native library (libpagespeed) is not loadable in this run.");
    }
}

/// <summary>
/// A theory that needs only the native library: skipped when the library is
/// not loadable in this run, unless
/// <see cref="NativeAssetsRequirement.RequiredOrCi"/> requires it (an explicit
/// <see cref="NativeAssetsRequirement.Variable"/>=1, or a CI run), in which
/// case the test runs and fails.
/// </summary>
public sealed class NativeLibTheoryAttribute : TheoryAttribute
{
    public NativeLibTheoryAttribute()
    {
        if (NativeAssetsRequirement.RequiredOrCi) return;
        if (!NativeLibAbsentFactAttribute.IsLibraryLoadable())
            Skip = NativeAssetsRequirement.SkipReason(
                "The native library (libpagespeed) is not loadable in this run.");
    }
}

/// <summary>
/// The inverse of <see cref="WorkerFactAttribute"/>: a fact for a test that
/// assumes the native library is NOT available (it asserts the behaviour of
/// the missing-library path). Skipped when the library is loadable in this run
/// by any route: the test output, the directory
/// <c>ImageVariantContentNegotiationTests.ResolveNativeDir</c> finds
/// (<c>PAGESPEED_E2E_NATIVE_DIR</c> or the NuGet cache), or the runtime's own
/// probing (app base, library search path, system path). A run with native
/// assets reports it as skipped, never as passed or failed because of the
/// library's presence.
/// </summary>
public sealed class NativeLibAbsentFactAttribute : FactAttribute
{
    public NativeLibAbsentFactAttribute()
    {
        if (IsLibraryLoadable())
            Skip = "The native library is loadable in this run; this test "
                + "covers the case where it is missing.";
    }

    internal static bool IsLibraryLoadable()
    {
        if (NativeAssetsFixture.NativeLibPresent()) return true;

        var dir = ImageVariantContentNegotiationTests.ResolveNativeDir(null);
        if (dir is not null && File.Exists(Path.Combine(
                dir, ImageVariantContentNegotiationTests.NativeLibName())))
            return true;

        // Goes through the registered resolver and the runtime's default
        // probing (app base root, LD_LIBRARY_PATH, system path).
        if (NativeLibrary.TryLoad("pagespeed",
                typeof(WeAmp.PageSpeed.PageSpeedCache).Assembly, null, out var handle))
        {
            NativeLibrary.Free(handle);
            return true;
        }
        return false;
    }
}
