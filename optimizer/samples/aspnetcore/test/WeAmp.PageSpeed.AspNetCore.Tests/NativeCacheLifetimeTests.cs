// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Text;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;
using Xunit.Abstractions;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Real native cache (libpagespeed): read results and writers keep the native
/// cache alive after the managed instance is disposed, which is what lets the
/// middleware retire a cache after a volume reset while responses still
/// stream from it.
///
/// SKIPS without native assets (see <see cref="NativeAssetsFixture"/>).
/// <see cref="ConcurrentReadsAcrossManySwaps"/> is a longer stress run and
/// is opt-in: set <c>PAGESPEED_LONG_TESTS=1</c> (trait <c>Category=Long</c>).
/// </summary>
[Collection(WorkerSubprocessCollection.Name)]
public sealed class NativeCacheLifetimeTests
{
    private readonly ITestOutputHelper _output;

    public NativeCacheLifetimeTests(ITestOutputHelper output) => _output = output;

    private static void Put(IPageSpeedCache cache, string url, byte[] body)
    {
        using var writer = cache.BeginWrite(url, "h", "https", new CacheWriteParams
        {
            ContentLength = (ulong)body.Length,
            FullMask = 0,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });
        writer.Write(body);
        writer.Commit();
    }

    private bool TryStage()
    {
        if (NativeAssetsFixture.TryStage(_output, out var skipReason)) return true;
        _output.WriteLine($"SKIP: {skipReason}");
        return false;
    }

    private static string NewCacheDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), $"ps_lifetime_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        return dir;
    }

    [Fact]
    public async Task ReadResultAndWriter_OutliveTheDisposedCache()
    {
        if (!TryStage()) return;
        var dir = NewCacheDir();
        try
        {
            var opts = new WeAmp.PageSpeed.PageSpeedCacheOptions
            {
                VolumePath = Path.Combine(dir, "volume.dat"),
                VolumeSizeBytes = 64 << 20,
            };
            var body = Encoding.UTF8.GetBytes(new string('x', 100_000));
            var cache = new PageSpeedCache(opts);
            Put(cache, "/a.css", body);
            var result = cache.ReadBest("/a.css", "h", "https", 0);
            Assert.NotNull(result);
            var memory = result!.ContentMemory;
            var writer = cache.BeginWrite("/b.css", "h", "https", new CacheWriteParams
            {
                ContentLength = 3,
                ContentType = PageSpeedContentType.Css,
            });

            cache.Dispose();
            Assert.Throws<ObjectDisposedException>(
                () => cache.ReadBest("/a.css", "h", "https", 0));
            await Task.Delay(50);
            GC.Collect();
            GC.WaitForPendingFinalizers();

            // Still mapped and still writable: the native cache was not closed.
            Assert.True(memory.Span.SequenceEqual(body));
            writer.Write(new byte[] { 1, 2, 3 });
            writer.Commit();
            writer.Dispose();
            result.Dispose();
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    [Trait("Category", "Long")]
    public async Task ConcurrentReadsAcrossManySwaps()
    {
        if (Environment.GetEnvironmentVariable("PAGESPEED_LONG_TESTS") != "1")
        {
            _output.WriteLine("SKIP: set PAGESPEED_LONG_TESTS=1 to run this stress test.");
            return;
        }
        if (!TryStage()) return;
        var dir = NewCacheDir();
        try
        {
            var opts = new WeAmp.PageSpeed.PageSpeedCacheOptions
            {
                VolumePath = Path.Combine(dir, "volume.dat"),
                VolumeSizeBytes = 64 << 20,
            };
            var genPath = opts.VolumePath + ".gen";
            var body = Encoding.UTF8.GetBytes(new string('y', 50_000));
            using (var seed = new PageSpeedCache(opts)) Put(seed, "/a.css", body);

            using var cache = new GenerationAwarePageSpeedCache(
                () => new PageSpeedCache(opts), genPath, logger: null, TimeSpan.Zero);
            using var stop = new CancellationTokenSource();
            var errors = new List<Exception>();
            long reads = 0, hits = 0;
            var readers = Enumerable.Range(0, 8).Select(_ => Task.Run(async () =>
            {
                while (!stop.IsCancellationRequested)
                {
                    try
                    {
                        using var result = cache.ReadBest("/a.css", "h", "https", 0);
                        Interlocked.Increment(ref reads);
                        if (result != null)
                        {
                            var memory = result.ContentMemory;
                            await Task.Yield();
                            if (!memory.Span.SequenceEqual(body))
                                throw new Exception("content mismatch");
                            Interlocked.Increment(ref hits);
                        }
                    }
                    catch (Exception ex)
                    {
                        lock (errors) errors.Add(ex);
                        return;
                    }
                }
            })).ToArray();

            for (ulong gen = 1; gen < 150; gen++)
            {
                var tmp = genPath + ".tmp";
                File.WriteAllText(tmp, $"{gen}\n");
                File.Move(tmp, genPath, overwrite: true);
                cache.CheckGenerationNow();
                await Task.Yield();
            }
            stop.Cancel();
            await Task.WhenAll(readers);

            _output.WriteLine($"reads={reads} hits={hits} gen={cache.CurrentGeneration}");
            foreach (var e in errors) _output.WriteLine(e.ToString());
            Assert.Empty(errors);
            Assert.Equal(149UL, cache.CurrentGeneration);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }
}
