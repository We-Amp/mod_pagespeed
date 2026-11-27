// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.IO;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Pin the version strings that ship with every release together.
///
/// As of v2.0.13 the C++ worker version is GENERATED from VERSION.txt by the
/// //src/product_version:gen_product_version Bazel genrule (version.h just
/// aliases kPageSpeedVersion = kProductVersion). So the worker version can no
/// longer drift from VERSION.txt by construction — there is no literal to keep
/// in sync. What remains hand-maintained is the NuGet package version
/// (samples/aspnetcore/Directory.Build.props &lt;Version&gt;), which must match
/// VERSION.txt; the release pipeline also asserts VERSION.txt == the git tag.
///
/// History: v2.0.5 shipped with the NuGet at 2.0.5 but the worker still
/// reporting 2.0.4 because a hand-edit was missed; v2.0.6 added this test;
/// v2.0.7..v2.0.12 then froze the worker literal at 2.0.7 anyway. The genrule
/// (v2.0.13) removes the hand-edit entirely, and the second test below guards
/// against anyone reintroducing a hardcoded worker version literal.
/// </summary>
public class VersionInvariantTests
{
    [Fact]
    public void NuGetProps_AndVersionFile_MustAgree()
    {
        var root = RepoRoot();

        var propsPath = Path.Combine(root, "samples", "aspnetcore", "Directory.Build.props");
        var verTxt    = Path.Combine(root, "VERSION.txt");

        Assert.True(File.Exists(propsPath), $"missing: {propsPath}");
        Assert.True(File.Exists(verTxt),    $"missing: {verTxt}");

        var propsMatch = Regex.Match(
            File.ReadAllText(propsPath),
            @"<Version>(?<v>[^<]+)</Version>");
        Assert.True(propsMatch.Success,
            $"<Version> element not found in {propsPath}");

        var nugetVersion = propsMatch.Groups["v"].Value.Trim();
        var fileVersion  = File.ReadAllText(verTxt).Trim();

        Assert.True(
            nugetVersion == fileVersion,
            "Version drift between release artifacts. " +
            $"samples/aspnetcore/Directory.Build.props <Version> = '{nugetVersion}'; " +
            $"VERSION.txt = '{fileVersion}'. Both must be bumped together " +
            "(the worker version is generated from VERSION.txt — see the genrule).");
    }

    /// <summary>
    /// Regression guard: the worker version must stay GENERATED from VERSION.txt
    /// via the gen_product_version genrule, never reintroduced as a hardcoded
    /// literal in version.h — that hardcoded literal is exactly the drift the
    /// genrule eliminated (frozen at 2.0.7 through v2.0.12).
    /// </summary>
    [Fact]
    public void WorkerVersion_MustBeGeneratedFromVersionFile_NotHardcoded()
    {
        var root = RepoRoot();
        var headerPath = Path.Combine(root, "src", "product_version", "version.h");
        Assert.True(File.Exists(headerPath), $"missing: {headerPath}");
        var header = File.ReadAllText(headerPath);

        // No SemVer string literal may be assigned directly to kPageSpeedVersion.
        // The genrule form is `kPageSpeedVersion = kProductVersion;` (no quote);
        // a regression would look like `kPageSpeedVersion[] = "2.0.x"`.
        var hardcoded = Regex.Match(header, "kPageSpeedVersion[^;\\n]*=\\s*\"\\d");
        Assert.False(hardcoded.Success,
            "version.h reintroduced a hardcoded kPageSpeedVersion literal; the "
            + "worker version must be generated from VERSION.txt by the "
            + "gen_product_version genrule (kPageSpeedVersion = kProductVersion).");

        // And it must actually be wired to the generated constant.
        Assert.True(
            header.Contains("kProductVersion")
                && header.Contains("product_version_string.h"),
            "version.h must include the generated product_version_string.h and "
            + "alias kPageSpeedVersion to kProductVersion.");
    }

    // CallerFilePath resolves to the absolute path of THIS source file at
    // compile time, which works in CI (the runner's workspace) and locally
    // alike. We then walk up four dirs to the repo root:
    //   samples/aspnetcore/test/WeAmp.PageSpeed.AspNetCore.Tests/<this file>
    //   └─ ../../../../ → repo root
    private static string RepoRoot([CallerFilePath] string f = "")
        => Path.GetFullPath(
            Path.Combine(Path.GetDirectoryName(f)!, "..", "..", "..", ".."));
}
