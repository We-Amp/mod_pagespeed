// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

var retry = () => { try { return attempt(); } catch (e) { return null; } };
