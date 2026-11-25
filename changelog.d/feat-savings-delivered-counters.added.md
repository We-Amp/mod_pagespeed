- **Statistics for optimized responses served from cache.** New variables on
  the statistics page count the sizes of optimized images, stylesheets and
  scripts each time one is sent on a rewritten URL, from the cache as well:
  `image_served_original_bytes` and `image_served_optimized_bytes`, and the same
  pair for `css` and `javascript`. They keep counting after the server is
  reloaded, or restarted with its cache kept, when every optimized resource is
  answered from the cache. The existing `*_total_bytes_saved` counters keep
  their meaning: bytes saved at the time a resource is optimized, once per
  resource. The admin console describes the new variables.
  Limits: the sizes in these variables are before compression. HEAD, range and
  conditional (304) requests are excluded. A response answered by a browser
  cache or by a cache in front of the server never reaches these counters.
  Combined stylesheets and scripts are usually not counted yet. Responses
  answered from the optimizer cache are counted in variables of their own,
  `ipro_daemon_served_*_bytes`.
