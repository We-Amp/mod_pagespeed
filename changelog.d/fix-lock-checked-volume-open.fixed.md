- **A second web server process can no longer pick up the cache file
  another one is discarding.** When the module and the optimizer disagree
  on the cache layout, the module discards the cache file its check
  created; until now a second worker process starting at that same moment
  could attach to that file instead and run without in-place optimization
  until it restarted. Opens of the shared cache volume are now coordinated
  between the web server's processes, so no process can pick up a file
  that is being discarded. On Windows a small hidden lock file
  (`.pagespeed-module-open.lock`) now appears in the optimizer's cache
  directory; it is expected and can be left in place.
