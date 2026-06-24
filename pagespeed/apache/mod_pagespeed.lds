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

    /* gRPC RTTI/vtable symbols needed by the controller's RPC layer.
       typeinfo/typestring must remain global for C++ RTTI to work
       correctly within the shared library. */
    _ZTI*grpc*;
    _ZTS*grpc*;

  local:
    *;
};
