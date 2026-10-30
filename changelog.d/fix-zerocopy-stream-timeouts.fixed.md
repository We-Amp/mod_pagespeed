- **Large cached responses served with `CycloneZeroCopyServe` over cleartext HTTP/2
  no longer stall after the first 64 KB** when the client reads quickly.
