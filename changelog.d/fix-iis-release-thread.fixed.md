- **On IIS, editing `pagespeed.config` while the site is serving traffic no
  longer risks a stalled or failed worker process.** The previous
  configuration is now retired in the background once its last request has
  finished, and other requests no longer wait while that happens. Also
  fixed: a worker process could fail at application pool recycle after a
  site whose `pagespeed.config` could not be read, either on first use or
  after an edit.
