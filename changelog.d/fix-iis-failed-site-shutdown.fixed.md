- **IIS no longer risks a worker process failure when a site whose module
  startup failed is recycled or its configuration is fixed.** A site that
  started with a `pagespeed.config` that failed initialization (for example
  a missing or unwritable `FileCachePath`) kept its half-initialized state
  in the worker process. Repairing the file or recycling the application
  pool released that state and could take the worker process down,
  counting towards rapid-fail protection. The state now starts out empty,
  so a failed startup is released cleanly.
