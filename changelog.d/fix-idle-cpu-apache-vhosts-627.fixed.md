- Apache: worker threads and idle wake-ups no longer grow with the number of
  server blocks that use the optimizer (on Apache, each virtual host is one).
  Virtual hosts that name the same `DaemonSocketPath` and `DaemonVolumePath`
  now share one connection to the optimizer's cache, so a child process runs
  one set of background threads per cache volume instead of one per virtual
  host (#627).
