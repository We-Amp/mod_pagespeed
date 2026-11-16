- After upgrading, optimized resources are recomputed once: each stylesheet,
  script and image is optimized again the first time it is requested, so
  expect extra CPU use, and some responses served as the original, until that
  completes.
  Origin files still fresh in the HTTP cache are not fetched again.
- Scripts that contain non-ASCII bytes are combined only when the script and
  the page are UTF-8; pages served as `application/ce-html+xml` are rewritten
  as XHTML; URLs inside CSS comments are no longer rewritten; and scripts
  inside `<noscript>`, `<noembed>` and `<noframes>` are no longer inlined or
  minified.
