- **Servers that ran with `FetchWithGzip on`: do not reuse their cache after a
  rollback.** Earlier versions could store responses whose body was not
  compressed although they were labelled gzip (see the fix for
  `FetchWithGzip on`). This version does not read cache entries written by
  earlier versions, so nothing needs to be done when upgrading. If you go back
  to an earlier version, clear the PageSpeed cache first, because that version
  reads its old entries again.
