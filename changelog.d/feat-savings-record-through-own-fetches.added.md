- **Served-size statistics count combined scripts and stylesheets that were minified first.**
  A combined script or stylesheet made from files that were minified before
  being combined now counts in `javascript_served_*_bytes` or
  `css_served_*_bytes` with the size of the files it started from, each time it
  is sent. Before, such a response was measured against its already-minified
  parts, so its saving was understated, and a combined script was usually not
  counted at all. The same holds for any optimized resource made from another
  optimized resource. A combination of files that were not optimized is still
  not counted: combining saves requests, not bytes.
