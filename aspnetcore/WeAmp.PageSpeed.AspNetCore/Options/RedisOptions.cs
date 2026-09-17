// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Redis cache backend configuration.
/// </summary>
public class RedisOptions
{
    /// <summary>
    /// Redis server hostname. Required to enable Redis caching.
    /// </summary>
    public string Host { get; set; } = "localhost";

    /// <summary>
    /// Redis server port. Default: 6379.
    /// </summary>
    public int Port { get; set; } = 6379;

    /// <summary>
    /// Timeout for Redis operations in microseconds.
    /// Default: 5000000 (5 seconds).
    /// </summary>
    public long TimeoutUs { get; set; } = 5000000;

    /// <summary>
    /// Delay between reconnection attempts in milliseconds.
    /// Default: 5.
    /// </summary>
    public long ReconnectionDelayMs { get; set; } = 5;

    /// <summary>
    /// Redis database index (0-15). Default: 0.
    /// </summary>
    public int DatabaseIndex { get; set; } = 0;

    /// <summary>
    /// TTL for cached items in seconds.
    /// Default: 86400 (24 hours).
    /// </summary>
    public int TtlSeconds { get; set; } = 86400;
}
