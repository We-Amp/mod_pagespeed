- The admin console now shows how much of the saving on responses served from
  the optimizer's cache came from compression applied by mod_pagespeed and how
  much from its optimization; your web server's own compression on other
  responses is not counted. The two parts, plus a share not yet attributed,
  add up to the total saving. The total now counts HTML that the optimizer
  makes larger, for example by inlining above-the-fold CSS or adding preload
  hints; earlier versions counted that as zero, so the total can read lower
  than before without anything optimizing less. After the update the caches
  start cold, so the split fills in as pages are re-optimized; until then the
  console shows the total only.
