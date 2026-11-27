// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

var drain = () => { while (queue.length) { queue.pop(); } };
