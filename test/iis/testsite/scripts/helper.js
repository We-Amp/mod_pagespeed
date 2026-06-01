/**
 * Helper functions for PageSpeed IIS test site.
 */

var PSHelper = (function() {
    'use strict';

    /**
     * Log a message with timestamp.
     * @param {string} message - The message to log.
     */
    function log(message) {
        var timestamp = new Date().toISOString();
        console.log('[' + timestamp + '] ' + message);
    }

    /**
     * Check if an element is in the viewport.
     * @param {Element} el - The element to check.
     * @return {boolean} True if visible.
     */
    function isInViewport(el) {
        var rect = el.getBoundingClientRect();
        return (
            rect.top >= 0 &&
            rect.left >= 0 &&
            rect.bottom <= window.innerHeight &&
            rect.right <= window.innerWidth
        );
    }

    /**
     * Debounce a function.
     * @param {Function} func - The function to debounce.
     * @param {number} wait - Wait time in milliseconds.
     * @return {Function} The debounced function.
     */
    function debounce(func, wait) {
        var timeout;
        return function() {
            var context = this;
            var args = arguments;
            clearTimeout(timeout);
            timeout = setTimeout(function() {
                func.apply(context, args);
            }, wait);
        };
    }

    return {
        log: log,
        isInViewport: isInViewport,
        debounce: debounce
    };
})();
