// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Text;

namespace WeAmp.PageSpeed;

/// <summary>
/// What the response-header gate decided about an origin response.
/// </summary>
public enum HeadersSidecarVerdict
{
    /// <summary>
    /// Every header is either reproduced by the cache entry, stamped fresh per
    /// response, or carried in the stored header block. The response may be
    /// cached.
    /// </summary>
    SwapEligible = 0,

    /// <summary>
    /// At least one header cannot be reproduced on a cache hit. The response
    /// is served from the origin and not cached.
    /// </summary>
    FallThrough = 1,

    /// <summary>
    /// A per-response value (a nonce in a Content-Security-Policy or Link
    /// header) makes the response unsuitable for caching.
    /// </summary>
    NeverOptimized = 2,
}

/// <summary>Outcome of <see cref="IPageSpeedCache.WriteHeadersSidecar"/>.</summary>
/// <param name="Verdict">The gate's decision.</param>
/// <param name="Stored">
/// True when a header block was stored. False for a response with nothing to
/// carry (all its headers are reproduced by the entry itself) or one the gate
/// refused.
/// </param>
public readonly record struct HeadersSidecarWriteResult(
    HeadersSidecarVerdict Verdict, bool Stored);

/// <summary>
/// Codec for the stored header block: the request-independent response
/// headers a cached entry replays on every hit.
/// </summary>
/// <remarks>
/// Payload layout (format version 1), the same as lib/cache/headers_sidecar.cc:
/// <c>[1B version][2B field count, big-endian]</c> then per field
/// <c>[1B name length][2B value length, big-endian][name][value]</c>.
/// Names and values are the origin's bytes; they round-trip through Latin-1.
/// </remarks>
public static class HeadersSidecar
{
    private const byte FormatVersion = 1;

    /// <summary>
    /// Parses a stored block. Returns false for an empty, truncated,
    /// unknown-version or over-long payload; the caller then behaves as on a
    /// cache miss.
    /// </summary>
    public static bool TryParse(
        ReadOnlySpan<byte> payload,
        out List<KeyValuePair<string, string>> fields)
    {
        fields = [];
        if (payload.Length < 3 || payload[0] != FormatVersion)
            return false;

        int count = (payload[1] << 8) | payload[2];
        int pos = 3;
        var parsed = new List<KeyValuePair<string, string>>(count);
        for (int i = 0; i < count; i++)
        {
            if (payload.Length - pos < 3) return false;
            int nameLen = payload[pos];
            int valueLen = (payload[pos + 1] << 8) | payload[pos + 2];
            pos += 3;
            if (nameLen == 0 || payload.Length - pos < nameLen + valueLen)
                return false;
            var name = Encoding.Latin1.GetString(payload.Slice(pos, nameLen));
            pos += nameLen;
            var value = Encoding.Latin1.GetString(payload.Slice(pos, valueLen));
            pos += valueLen;
            parsed.Add(new KeyValuePair<string, string>(name, value));
        }
        if (pos != payload.Length) return false;

        fields = parsed;
        return true;
    }
}
