// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed E2E Test Script - intentionally unminified

/**
 * Main application entry point.
 * This script demonstrates JS minification through the pipeline.
 */
function  initApp()  {
  // Log startup
  console.log( 'PageSpeed E2E test app initialized' );

  // Set up event listeners
  var  hero  =  document.querySelector( '.hero' );
  if  ( hero )  {
    hero.addEventListener( 'click',  function()  {
      console.log( 'Hero section clicked' );
    });
  }

  // Track page load time
  var  loadTime  =  performance.now();
  console.log( 'Page loaded in '  +  loadTime.toFixed(2)  +  'ms' );
}

// Initialize when DOM is ready
if  ( document.readyState  ===  'loading' )  {
  document.addEventListener( 'DOMContentLoaded',  initApp );
}  else  {
  initApp();
}
