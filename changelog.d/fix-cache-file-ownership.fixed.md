- A start or config test as root no longer leaves a cache file the workers
  cannot use: nginx and Apache started as root create the cache files for
  the worker user (`user` in nginx, `User`/`Group` in Apache), also when an
  earlier root start left a root-owned volume file behind. A volume path that
  is not a plain file is reported and not used. A cache directory configured
  as a symbolic link is left as it is, with a warning.
