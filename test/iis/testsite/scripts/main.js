/**
 * Main JavaScript file for PageSpeed IIS test site.
 */

(function() {
    'use strict';

    /**
     * Initialize the page.
     */
    function init() {
        console.log('PageSpeed IIS Test Site initialized');
        addEventListeners();
    }

    /**
     * Add event listeners.
     */
    function addEventListeners() {
        document.addEventListener('DOMContentLoaded', function() {
            console.log('DOM fully loaded');
        });
    }

    // Initialize on load
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
