- **Statistics for optimized responses served from cache.** The statistics page
  has new variables that count the sizes of optimized images, stylesheets and
  scripts each time one is sent on a rewritten URL, from the cache as well:
  `image_served_original_bytes` and `image_served_optimized_bytes`, and the same
  pair for `css` and `javascript`. Before, the only sizes shown were the totals
  the filters keep, which grow once when a resource is optimized, so a server
  that answered everything from its cache showed no savings after a restart.
  Sizes are of uncompressed bodies. `served_savings_unmeasured_responses` counts
  optimized responses sent from cache entries written by an earlier release,
  which carry no sizes. Resources optimized in place under their own URL are
  not counted yet.
