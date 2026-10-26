- **nginx: reload, worker exit and Web Bot Auth key-file robustness.**
  Reloading with `master_process off` no longer crashes: the reload is
  refused with an error and pagespeed stays off until nginx is restarted; a
  worker exiting with work outstanding no longer crashes; the Web Bot Auth
  key file is rechecked at most every 5 seconds instead of on every signed
  request.
