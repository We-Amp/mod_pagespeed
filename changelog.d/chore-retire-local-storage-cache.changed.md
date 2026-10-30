- **`local_storage_cache` is retired; it is accepted and ignored with a warning.**
  The filter kept inlined CSS and images in the browser's local storage and
  tracked them in a cookie that could grow until the server refused requests
  with "400 request header too large". Existing configurations keep loading:
  the filter name is still accepted in every configuration syntax, in
  `EnableFilters` and in query parameters, and now logs one warning that it has
  no effect. Pages no longer get the local-storage cookie or the inline script,
  and the `num_local_storage_cache_*` statistics are gone. Remove the filter
  from your configuration. Reported in the archived upstream repository:
  https://github.com/apache/incubator-pagespeed-mod/issues/1528,
  https://github.com/apache/incubator-pagespeed-mod/issues/1275 and
  https://github.com/apache/incubator-pagespeed-mod/issues/1880
