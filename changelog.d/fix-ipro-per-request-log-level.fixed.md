- **In-place optimization no longer floods the message history.** Every
  in-place hit and miss used to write an informational line into the shared
  message history, pushing real warnings and errors out of the console's
  Messages page within seconds on a busy site. Those per-request lines are now
  written at debug level on nginx, Envoy and IIS (visible with the debug log
  level); the `ipro_*` statistics remain the counters. Reported at
  https://github.com/apache/incubator-pagespeed-ngx/issues/1720.
