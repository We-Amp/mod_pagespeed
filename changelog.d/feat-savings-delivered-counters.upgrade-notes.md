- **The optimization cache starts empty after upgrading to this version.**
  Entries written by earlier versions are no longer used: every resource is
  fetched and optimized again once, the first time it is needed, and the old
  entries age out of the cache on their own. Expect more fetches from the
  origin and more optimization work for a while after the upgrade. Nothing
  needs to be done. The entries of the previous version are no longer used but
  keep occupying cache space, within the configured cache size limit, until
  normal eviction removes them; to get the space back at once, clear the
  PageSpeed cache directory while the server is stopped. Servers of this
  version and of an earlier one that share a cache no longer read each other's
  entries.
