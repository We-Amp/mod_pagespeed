- **nginx now accepts deprecated option names with a warning, as Apache does,
  instead of refusing to start.** A configuration that still names a removed
  option, such as `pagespeed FileCacheInodeLimit` or
  `pagespeed FileCacheCleanIntervalMs`, used to fail `nginx -t` after an
  upgrade. Such names are now ignored, and the configuration test logs one
  warning per name saying it is deprecated and has no effect. Names that were
  never options still fail the configuration test.
