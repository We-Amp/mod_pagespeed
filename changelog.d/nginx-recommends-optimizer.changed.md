- **The nginx module package now recommends the matching optimizer.** The deb
  and rpm of `nginx-module-pagespeed` recommend `pagespeed-optimizer` at the
  same release, so a normal `apt install` or `dnf install` pulls the worker in.
  It stays optional: with `--no-install-recommends` or
  `--setopt=install_weak_deps=False` nginx runs with classic in-place
  optimization.
