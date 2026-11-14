- Apache: idle CPU and thread count in each child process no longer grow with
  the number of virtual hosts that use the optimizer. Virtual hosts that name
  the same `DaemonSocketPath` and `DaemonVolumePath` now share one connection
  to the optimizer's cache, so a child runs one set of background threads per
  cache volume instead of one per virtual host (#627).
