- **The recorder now recognises stacked content codings** on the
  optimizer-served path as well, so a response in a coding it cannot decode is
  never recorded as plain.
- **nginx no longer logs a misleading warning** when in-place recording is
  skipped for a response in a content coding it cannot decode.
