// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Admin endpoint authentication configuration.
/// Controls access to /pagespeed_admin and /pagespeed_statistics.
/// </summary>
public class AdminAuthOptions
{
    /// <summary>
    /// Whether admin authentication is enabled. Default: true.
    /// </summary>
    public bool Enabled { get; set; } = true;

    /// <summary>
    /// Bearer token for authentication. If not set, a random token is generated.
    /// Clients must provide this in the Authorization header:
    /// "Authorization: Bearer {token}"
    /// </summary>
    public string? Token { get; set; }

    /// <summary>
    /// Allowed source IPs/CIDRs for the admin endpoints (/pagespeed_admin,
    /// /pagespeed_statistics, /pagespeed_console, /pagespeed_message). The
    /// generated nginx config emits an "allow &lt;cidr&gt;; ... deny all;" gate from
    /// this list. Default: loopback only — the
    /// admin surface exposes powerful endpoints and must not reach untrusted
    /// networks. Widen ONLY if you understand the exposure; every entry is
    /// validated as an IP/CIDR (fail-closed) before it reaches the config.
    /// </summary>
    public List<string> AllowedIps { get; set; } =
    [
        "127.0.0.1/32",
        "::1/128"
    ];

    /// <summary>
    /// Rate limit for the admin endpoints in requests per minute, per client IP.
    /// Default: 60. Set to 0 to disable. The generated nginx config emits a
    /// <c>limit_req_zone</c> (http scope) + a <c>limit_req ... burst=10 nodelay</c> on
    /// each admin location, so a small burst is served immediately and sustained
    /// access is capped at this rate. This bounds the rate of <b>authenticated</b>
    /// admin traffic (protecting the expensive admin/stats handlers from a runaway or
    /// compromised loopback client); it does <b>not</b> throttle unauthenticated
    /// brute-force, because the bearer gate rejects those in nginx's rewrite phase
    /// before the limiter's preaccess phase runs — that surface is already denied
    /// cheaply by the loopback ACL + bearer. Defense-in-depth on top of those gates.
    /// Out-of-range values (&gt; 1,000,000) fail config generation closed.
    /// </summary>
    public int RateLimitRpm { get; set; } = 60;
}
