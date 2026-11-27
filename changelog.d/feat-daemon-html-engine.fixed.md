- **A URL purge matches the URL however its host is spelled.** A purge of
  one URL, from the admin console or the PURGE method, now also purges the
  URL with its host in lowercase and a default port (`:80` for http, `:443`
  for https) removed, so purging `http://Example.com:80/page` also covers
  `http://example.com/page`. An already normal URL still takes one entry in
  the purge set.
