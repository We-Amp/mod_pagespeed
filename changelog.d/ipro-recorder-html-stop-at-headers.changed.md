- **The in-place recorder no longer holds HTML responses in memory.** With the
  optimizer service in use, Apache, nginx and IIS used to accumulate each HTML
  response they passed through the in-place recorder before refusing it, since
  HTML is never recorded there. The recorder now decides from the response
  headers and holds none of the page. Recording of stylesheets, scripts and
  images is unchanged.
