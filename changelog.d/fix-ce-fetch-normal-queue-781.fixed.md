- Cache-extended stylesheets and scripts no longer wait for image optimization
  to finish. Before, a page with many images to optimize could hold back its
  render-blocking stylesheets and scripts for seconds on the first visits;
  they are now served right away.
