- **ASP.NET Core sidecar: invalid timing options fail at startup, a failed
  sidecar stops repeating its error, health probes only accept this app's own
  sidecar, and the per-start socket temp directory is removed on shutdown.**
  `PageSpeed:Sidecar:HealthCheckIntervalMs`, `HealthCheckTimeoutMs` and
  `ShutdownTimeoutMs` must be positive and at most ten minutes.
