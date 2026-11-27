// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

namespace WeAmp.PageSpeed;

public enum PageSpeedError
{
    Ok = 0,
    NotFound = 1,
    IoError = 2,
    Corrupted = 3,
    NoSpace = 4,
    InvalidArgument = 5,
    Busy = 6,
    Closed = 7,
    TooManyAlternates = 8,
    Exists = 9,
    NotOwned = 10,
    VersionMismatch = 11,
    Internal = 99,
}

/// <summary>High-level content category for cache metadata.</summary>
public enum PageSpeedContentType
{
    Html = 0,
    Css = 1,
    Js = 2,
    Image = 3,
    Other = 4,
}

/// <summary>Device viewport class for capability mask classification.</summary>
public enum PageSpeedViewport
{
    Mobile = 0,
    Tablet = 1,
    Desktop = 2,
}
