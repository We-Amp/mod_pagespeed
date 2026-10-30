- **Image handlers no longer throw "pagespeed is not defined" when the support script is missing.**
  The `onload` and `onerror` handlers that the critical-image beacon, lazy-load and delay-images
  filters add to `<img>` elements now check that the support script ran before calling it, so a
  page where that script is blocked, deferred or stripped by another tool no longer logs a
  `ReferenceError` for every image. Originally reported at
  https://github.com/apache/incubator-pagespeed-mod/issues/1316 and
  https://github.com/apache/incubator-pagespeed-mod/issues/1977.
