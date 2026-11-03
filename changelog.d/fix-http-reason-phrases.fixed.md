- **Status 307 and 505 responses now carry their standard reason phrases.** Where the module sets the
  reason phrase from the status code, a 307 response reported `OK` and a 505 response reported
  `Internal Server Error`; they now read `Temporary Redirect` and `HTTP Version Not Supported`. A
  Content-Type merged from a page's meta tag that names no charset no longer ends in a stray `;`.
