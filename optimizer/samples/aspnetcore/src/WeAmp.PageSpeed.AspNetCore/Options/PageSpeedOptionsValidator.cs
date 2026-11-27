// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.Options;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Validates <see cref="PageSpeedOptions"/> at startup and on hot-reload.
/// </summary>
/// <remarks>
/// This validator is the AOT/trim-safe replacement for
/// <c>ValidateDataAnnotations()</c>. It enforces the constraints expressed
/// as <c>[Required]</c>/<c>[Range]</c> on <see cref="PageSpeedCacheOptions"/>
/// in code, so the options pipeline does not need to reflect over attributes
/// at runtime (which trips IL2026 under trimming).
/// </remarks>
public sealed class PageSpeedOptionsValidator : IValidateOptions<PageSpeedOptions>
{
    /// <inheritdoc/>
    public ValidateOptionsResult Validate(string? name, PageSpeedOptions options)
    {
        var failures = new List<string>();

        // Cache options — mirrors the [Required]/[Range] attributes on
        // PageSpeedCacheOptions. Kept in sync by code review; the attributes
        // remain on the type as documentation.
        if (string.IsNullOrEmpty(options.Cache.VolumePath))
            failures.Add("Cache.VolumePath is required.");

        if (options.Cache.VolumeSizeBytes < 1)
            failures.Add($"Cache.VolumeSizeBytes must be >= 1, got {options.Cache.VolumeSizeBytes}.");

        if (options.Cache.RamCacheSizeBytes < 0)
            failures.Add($"Cache.RamCacheSizeBytes must be non-negative, got {options.Cache.RamCacheSizeBytes}.");

        if (options.Cache.MaxMetadataSizeBytes < 0)
            failures.Add($"Cache.MaxMetadataSizeBytes must be non-negative, got {options.Cache.MaxMetadataSizeBytes}.");

        if (!Enum.IsDefined(options.CacheMode))
            failures.Add($"CacheMode value '{(int)options.CacheMode}' is not a valid CacheMode.");

        if (options.CssMaxAgeSeconds < 0)
            failures.Add($"CssMaxAgeSeconds must be non-negative, got {options.CssMaxAgeSeconds}.");

        if (options.ImageMaxAgeSeconds < 0)
            failures.Add($"ImageMaxAgeSeconds must be non-negative, got {options.ImageMaxAgeSeconds}.");

        if (options.HtmlMaxAgeSeconds < 0)
            failures.Add($"HtmlMaxAgeSeconds must be non-negative, got {options.HtmlMaxAgeSeconds}.");

        if (!string.IsNullOrEmpty(options.AgentContentSignal) &&
            !Internal.AgentMarkdownHeaders.IsValidContentSignalValue(options.AgentContentSignal))
            failures.Add(
                "AgentContentSignal must be 1 to " +
                $"{Internal.AgentMarkdownHeaders.MaxContentSignalBytes} characters of printable " +
                "ASCII on one line, not starting or ending with a space or tab.");

        return failures.Count > 0
            ? ValidateOptionsResult.Fail(failures)
            : ValidateOptionsResult.Success;
    }
}
