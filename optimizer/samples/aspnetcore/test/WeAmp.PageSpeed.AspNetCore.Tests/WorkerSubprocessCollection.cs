// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Non-parallel collection for the full-stack tests that boot a real
/// <c>factory_worker</c> subprocess and drive image/HTML transcoding through
/// it. xUnit parallelizes test classes by default, so without this these
/// tests would run concurrently — each spawning its own worker and competing
/// for the worker thread pool / CPU. Image transcoding (WebP/AVIF +
/// ssimulacra2 quality search) is CPU-intensive (especially in Debug builds
/// and on loaded CI runners), so under contention the variant doesn't
/// materialize within the test's poll ceiling and the test times out — a
/// load flake, not a regression (cf. the 1.1 IIS init-load-flake pattern).
///
/// Putting every worker-subprocess test in this single collection serializes
/// them, so each gets the full machine while it runs. Members:
/// <see cref="ImageVariantContentNegotiationTests"/>,
/// <see cref="WorkerSocketPathDefaultTests"/>,
/// <see cref="ServeStatsParityTests"/>.
/// </summary>
[CollectionDefinition(Name)]
public sealed class WorkerSubprocessCollection
{
    public const string Name = "WorkerSubprocess";
}
