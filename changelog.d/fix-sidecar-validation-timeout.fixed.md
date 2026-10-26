- **The ASP.NET Core sidecar no longer leaks a stuck `nginx -t` config
  validation.** When the pre-launch `nginx -t` did not finish within its
  budget, startup failed with a bare cancellation error, the validation process
  was left running and whatever it had printed was discarded. The sidecar now
  kills the process (a cancelled application shutdown kills it too) and fails
  with a timeout message that reports the elapsed time and the output captured
  so far. The budget is configurable through
  `PageSpeed:Sidecar:ConfigValidationTimeoutMs` (default 60 seconds, previously
  a fixed 15 seconds); `nginx -t` on a healthy host finishes well under a
  second, so the new default only changes behaviour on hosts too loaded to
  finish at all. Non-positive values are rejected at startup.
