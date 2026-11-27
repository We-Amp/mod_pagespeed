// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>Native library version information.</summary>
public static class PageSpeedVersion
{
    public static Version Current => new(
        NativePageSpeed.VersionMajor(),
        NativePageSpeed.VersionMinor(),
        NativePageSpeed.VersionPatch());
}

/// <summary>Validates native library version at startup.</summary>
internal static class VersionCheck
{
    private static readonly Version MinimumNative = new(1, 0, 0);

    internal static void EnsureCompatible()
    {
        var native = PageSpeedVersion.Current;
        if (native.Major != MinimumNative.Major)
            throw new PageSpeedException(
                PageSpeedError.VersionMismatch,
                $"Native library {native} is incompatible " +
                $"(requires major version {MinimumNative.Major})");
    }
}
