- IIS: the install guide now explains how the module works with IIS output-cache
  profiles. A profile without `location="Any"` or `location="Downstream"` makes IIS
  send `Cache-Control: no-cache` or `private`, so the stylesheets, scripts and images
  it covers are served unoptimized; the guide says how to set the profile so they are
  optimized in place.
