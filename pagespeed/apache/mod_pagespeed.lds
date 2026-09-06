/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024-2026 We-Amp B.V.
 */

{
  global:
    /* Apache module entry point */
    pagespeed_module;

    /* APR/APR-Util symbols - must remain global so the dynamic linker
       resolves them to Apache's copies (not the statically linked ones) */
    apr_*;
    apu_*;

    /* Apache API symbols */
    ap_*;

  local:
    *;
};
