- **Lower request latency on Apache and IIS with the optimizer worker.**
  Requests no longer wait on one another to reach the optimizer cache. While
  the cache is being opened, for example right after a full cache purge,
  other requests are answered by the origin instead of waiting. When the
  optimizer worker was not running at server start, the periodic check for it
  no longer holds a request for up to two seconds.
