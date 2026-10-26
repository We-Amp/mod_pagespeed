- **IIS event-log entries for an unusable optimizer service always name
  the cause.** A second configuration that hits an already announced cause
  now gets an entry naming its own cause, and an unset
  `DaemonSocketPath` or `DaemonVolumePath` is shown as `(not set)`.
