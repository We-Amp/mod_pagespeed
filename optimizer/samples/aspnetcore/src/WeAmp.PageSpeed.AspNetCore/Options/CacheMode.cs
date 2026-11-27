// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Controls Cache-Control header behaviour for cache HIT responses.
/// </summary>
public enum CacheMode
{
    /// <summary>
    /// Conservative caching: must-revalidate on assets, no-cache on HTML.
    /// Safe default that avoids stale content.
    /// </summary>
    Safe = 0,

    /// <summary>
    /// Aggressive caching: public + stale-if-error on assets, no-cache on HTML.
    /// Higher performance but may serve stale content during origin errors.
    /// </summary>
    Aggressive = 1,
}
