- Fixed: with the Web Bot Auth counter on, `nginx -t` no longer creates the
  counter file in the cache directory, and nginx started as root gives that
  file to the worker user (`user`), as it does the cache files, also when an
  earlier root start left a root-owned one behind.
