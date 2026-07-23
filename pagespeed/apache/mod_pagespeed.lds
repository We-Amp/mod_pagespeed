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
