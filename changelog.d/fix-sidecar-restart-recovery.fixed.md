- **ASP.NET Core sidecar: a restart that misses its startup deadline is
  stopped and retried, and shutdown stops a restarting sidecar.** A restart
  that missed its startup deadline while the new optimizer process was still
  running used to leave the manager parked in "restarting" for the life of
  the app: requests bypassed optimization, health checks reported degraded
  and shutdown skipped the graceful stop. The slow process is now stopped and
  restarted again, each attempt counting toward the restart-attempt limit, so
  a host whose restarts are always slow ends in the failed state instead of
  a silent bypass: raise `StartupTimeoutMs` on such hosts. A restarting
  sidecar is stopped on shutdown like any other state.
