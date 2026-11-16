- **nginx now accepts a deprecated option name only where the option itself was
  valid.** Since deprecated names were allowed with a warning, a server-level
  name such as `pagespeed FileCacheInodeLimit` was also silently accepted inside
  a `location` block. It is now refused there, as in Apache.
