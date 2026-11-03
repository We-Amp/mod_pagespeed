- **PageSpeed option values wrapped in more than one layer of quotes are now read correctly.** A
  value in a query parameter, cookie or header that ends in two or more layers of quotes, such as
  `PageSpeedFilters=''rewrite_css''`, kept a layer of quotes and was not applied; it is now fully
  unwrapped. Values with one layer of quotes behave as before.
