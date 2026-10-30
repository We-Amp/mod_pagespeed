- **Inline scripts added by the image and iframe filters no longer throw when the support script is missing.**
  The small scripts that the lazy-load, delay-images, inline-image deduplication and deferred-iframe
  filters add to the page now check that the support script ran before calling it, so a page where
  that script is blocked, deferred or stripped by another tool no longer logs a `ReferenceError`.
  Deduplicating an inlined image now needs the image to be a little larger before it pays off, because
  the restoring script is slightly longer.
