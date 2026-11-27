// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Which request host names AllowedHosts stands behind for agent optimization.
/// </summary>
public sealed class AgentHostVouchTests
{
    [Theory]
    [InlineData("example.com", "example.com", true)]
    [InlineData("Example.COM", "example.com", true)]
    [InlineData("example.com:8080", "example.com", true)]
    [InlineData("example.com", "www.example.com", false)]
    [InlineData("*.example.com", "www.example.com", true)]
    [InlineData("*.example.com", "a.b.example.com", true)]
    [InlineData("*.example.com", "example.com", false)]
    [InlineData("*.example.com", "badexample.com", false)]
    [InlineData("[::1]", "[::1]", true)]
    public void ListedNamesAreVouched(string allowed, string host, bool expected)
    {
        Assert.Equal(expected, AgentHostVouch.Vouches(new[] { allowed }, host));
    }

    [Theory]
    [InlineData("*")]
    [InlineData("[::]")]
    [InlineData("0.0.0.0")]
    public void AnyHostEntryVouchesForNothing(string anyHost)
    {
        Assert.False(AgentHostVouch.Vouches(
            new[] { "example.com", anyHost }, "example.com"));
    }

    [Fact]
    public void NoListVouchesForNothing()
    {
        Assert.False(AgentHostVouch.Vouches(null, "example.com"));
        Assert.False(AgentHostVouch.Vouches(Array.Empty<string>(), "example.com"));
        Assert.False(AgentHostVouch.Vouches(new[] { "example.com" }, ""));
    }
}
