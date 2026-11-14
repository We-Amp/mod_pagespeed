- nginx: idle CPU and thread count in each worker no longer grow with the
  number of server blocks that use the optimizer. Server blocks that name the
  same `DaemonSocketPath` and `DaemonVolumePath` now share one connection to
  the optimizer's cache, so a worker runs one set of background threads per
  cache volume instead of one per server block (#627).
