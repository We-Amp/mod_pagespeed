// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed E2E Interactive Test Script - intentionally unminified
//
// Tests DOM manipulation, event handlers, and dynamic element creation
// through the minification pipeline.

/**
 * Set up DOM manipulation tests.
 * Updates  #js-output  text  to  confirm  JS  execution.
 */
function  setupDomTests()  {
  var  output  =  document.getElementById( 'js-output' );
  if  ( output )  {
    output.textContent  =  'JavaScript executed successfully';
    output.setAttribute( 'data-js-loaded',  'true' );
  }
}

/**
 * Set up event handler tests.
 * Clicking  #test-button  changes  its  text  and  sets  data-clicked.
 */
function  setupEventHandlers()  {
  var  button  =  document.getElementById( 'test-button' );
  if  ( button )  {
    button.addEventListener( 'click',  function()  {
      button.textContent  =  'Button was clicked';
      button.setAttribute( 'data-clicked',  'true' );
    });
  }
}

/**
 * Create dynamic DOM elements.
 * Adds  3  list  items  to  #dynamic-list.
 */
function  createDynamicElements()  {
  var  list  =  document.getElementById( 'dynamic-list' );
  if  ( list )  {
    for  ( var  i  =  1;  i  <=  3;  i++ )  {
      var  li  =  document.createElement( 'li' );
      li.textContent  =  'Dynamic item '  +  i;
      li.className  =  'dynamic-item';
      list.appendChild( li );
    }
  }
}

/**
 * Main  interactive  initialization.
 */
function  initInteractive()  {
  setupDomTests();
  setupEventHandlers();
  createDynamicElements();
  console.log( 'Interactive JS fully loaded' );
}

// Initialize when DOM is ready
if  ( document.readyState  ===  'loading' )  {
  document.addEventListener( 'DOMContentLoaded',  initInteractive );
}  else  {
  initInteractive();
}
