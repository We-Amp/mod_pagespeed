- On nginx, `expires` and `add_header Cache-Control` no longer give a longer
  lifetime to an image served for another device class. That response goes out
  with one `Cache-Control` line: every configured directive is kept and each
  `max-age` and `s-maxage` is shortened to 60 seconds or less. Responses
  marked `private`, `no-store` or `no-cache`, or with an unreadable lifetime,
  are left as configured, and all other responses keep the lifetime they had.
