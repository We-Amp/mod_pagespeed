// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Windows: the access rules the worker puts on its named pipes.

#ifndef SRC_WORKER_PIPE_DACL_H_
#define SRC_WORKER_PIPE_DACL_H_

#include <string>

namespace pagespeed {

// FILE_GENERIC_READ | FILE_GENERIC_WRITE without FILE_CREATE_PIPE_INSTANCE:
// a client may read and write, and nothing else.
inline constexpr unsigned long kPipeClientAccessMask = 0x12019b;

// The SDDL for the notification and health pipes: SYSTEM and the worker's
// own account full control (libuv adds the pipe's further instances after
// the rules are set, so the worker's account must keep that right),
// Authenticated Users read and write only. Empty when the process's own
// account cannot be read.
std::string OpenPipeSddl();

// The SDDL for the management pipe: SYSTEM, Administrators and the worker's
// own account. Empty when the process's own account cannot be read.
std::string RestrictedPipeSddl();

}  // namespace pagespeed

#endif  // SRC_WORKER_PIPE_DACL_H_
