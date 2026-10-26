- **`nginx -t` no longer creates the file cache's volume files.** A
  configuration test now only validates the configuration, so it no longer
  takes a minute on a slow disk or times out the ASP.NET Core sidecar's startup
  validation. Normal startup still creates and checks the volumes.
