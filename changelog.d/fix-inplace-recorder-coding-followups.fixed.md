- **The recorder now recognises stacked content codings** on the
  optimizer-served path as well, so a response in a coding it cannot decode is
  never recorded as plain.
- **nginx no longer logs a misleading "incomplete/truncated response"
  warning** when in-place recording skips a response early, for example one
  whose content type is not optimized or whose content coding the server
  cannot decode.
