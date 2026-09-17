/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024-2026 We-Amp B.V.
 */

{
  global:
    /* nginx dynamic module entry points (dlsym) */
    ngx_modules;
    ngx_module_names;
    ngx_module_order;

  local:
    *;
};
