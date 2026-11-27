// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The regression gate for the worker that would not start under the NuGet
/// host.
///
/// The optimizer daemon validates its cache directory at startup and refuses
/// to start when it is absent: it never creates the directory, never chowns,
/// and never falls back to another location. In the packaged
/// deployment the directory arrives ahead of the service from tmpfiles.d. An
/// application that embeds the worker has no packaging step, so
/// <see cref="WorkerProcessHost"/> must provision the directory itself before
/// it spawns the worker.
///
/// Before the fix nothing did that deliberately: the directory happened to
/// exist because <c>PageSpeedCache</c>'s constructor creates the same parent,
/// and whichever of the two ran first decided whether the worker came up. The
/// worker started on one run and exited at startup on the next, leaving the
/// API port unreachable and no optimized variants.
/// </summary>
public class WorkerCacheDirectoryTests
{
    [Fact]
    public void EnsureCacheDirectory_CreatesMissingParent()
    {
        var root = NewScratchRoot();
        try
        {
            var volumePath = Path.Combine(root, "cache", "volume.dat");
            Assert.False(Directory.Exists(Path.GetDirectoryName(volumePath)));

            WorkerProcessHost.EnsureCacheDirectory(volumePath, NullLogger.Instance);

            Assert.True(
                Directory.Exists(Path.GetDirectoryName(volumePath)),
                "the worker refuses to start without its cache directory, so "
                + "the host must create it before spawning the worker");
        }
        finally
        {
            TryDelete(root);
        }
    }

    [Fact]
    public void EnsureCacheDirectory_CreatesNestedParents()
    {
        var root = NewScratchRoot();
        try
        {
            var volumePath = Path.Combine(root, "a", "b", "c", "volume.dat");

            WorkerProcessHost.EnsureCacheDirectory(volumePath, NullLogger.Instance);

            Assert.True(Directory.Exists(Path.GetDirectoryName(volumePath)));
        }
        finally
        {
            TryDelete(root);
        }
    }

    [Fact]
    public void EnsureCacheDirectory_CreatesOwnerOnlyOnUnix()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return; // Access is an ACL question on Windows, not a mode.

        var root = NewScratchRoot();
        try
        {
            var volumePath = Path.Combine(root, "cache", "volume.dat");

            WorkerProcessHost.EnsureCacheDirectory(volumePath, NullLogger.Instance);

            var dir = Path.GetDirectoryName(volumePath)!;
            var mode = File.GetUnixFileMode(dir);

            // The volume, the shared config and the serve-stats mmap all live
            // in here and are only ever shared with this application's own
            // worker, running as the same user. Nothing outside needs a bit.
            Assert.Equal(
                UnixFileMode.UserRead | UnixFileMode.UserWrite
                    | UnixFileMode.UserExecute,
                mode);
        }
        finally
        {
            TryDelete(root);
        }
    }

    [Fact]
    public void EnsureCacheDirectory_LeavesAnExistingDirectoryAlone()
    {
        var root = NewScratchRoot();
        try
        {
            var dir = Path.Combine(root, "cache");
            Directory.CreateDirectory(dir);

            UnixFileMode before = default;
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                // An operator-provisioned directory that is group-readable on
                // purpose must not be narrowed behind their back.
                before = UnixFileMode.UserRead | UnixFileMode.UserWrite
                    | UnixFileMode.UserExecute | UnixFileMode.GroupRead
                    | UnixFileMode.GroupExecute;
                File.SetUnixFileMode(dir, before);
            }

            WorkerProcessHost.EnsureCacheDirectory(
                Path.Combine(dir, "volume.dat"), NullLogger.Instance);

            Assert.True(Directory.Exists(dir));
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                Assert.Equal(before, File.GetUnixFileMode(dir));
        }
        finally
        {
            TryDelete(root);
        }
    }

    [Fact]
    public void EnsureCacheDirectory_UncreatableParentDoesNotThrow()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return;
        if (Environment.GetEnvironmentVariable("USER") == "root")
            return; // root can write anywhere; the leg proves nothing.

        var root = NewScratchRoot();
        try
        {
            var locked = Path.Combine(root, "locked");
            Directory.CreateDirectory(locked);
            File.SetUnixFileMode(
                locked, UnixFileMode.UserRead | UnixFileMode.UserExecute);

            // The worker is a best-effort background service: a cache
            // directory that cannot be made is reported, never thrown out of
            // the host's startup path.
            WorkerProcessHost.EnsureCacheDirectory(
                Path.Combine(locked, "cache", "volume.dat"), NullLogger.Instance);

            File.SetUnixFileMode(
                locked,
                UnixFileMode.UserRead | UnixFileMode.UserWrite
                    | UnixFileMode.UserExecute);
        }
        finally
        {
            TryDelete(root);
        }
    }

    [Fact]
    public void EnsureCacheDirectory_BarePathWithNoDirectoryIsIgnored()
    {
        // Path.GetDirectoryName returns "" here; there is nothing to create
        // and nothing to fail on.
        WorkerProcessHost.EnsureCacheDirectory("volume.dat", NullLogger.Instance);
    }

    private static string NewScratchRoot()
    {
        var root = Path.Combine(
            Path.GetTempPath(), $"ps_cachedir_{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        return root;
    }

    private static void TryDelete(string path)
    {
        try { Directory.Delete(path, recursive: true); } catch { }
    }
}
