- **The savings statistics are documented.** `docs/operations-monitoring.md`
  has a new section that explains the two kinds of savings numbers: saved at
  rewrite time (`*_total_bytes_saved`, once per resource) and delivered
  (`*_served_*_bytes` and `ipro_daemon_served_*_bytes`, each time an optimized
  response is sent). It covers when the optimizer-cache figures are in sizes
  after compression, which requests and responses are not counted, that the
  counters start from zero at a restart and how the admin console shows that,
  the two settings statistics logging needs, and that the module's cache starts
  empty after upgrading.
