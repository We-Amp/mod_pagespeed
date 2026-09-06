// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Utility functions for PageSpeed IIS test site.
 */

var PSUtil = (function() {
    'use strict';

    /**
     * Format a date as ISO string.
     * @param {Date} date - The date to format.
     * @return {string} The formatted date string.
     */
    function formatDate(date) {
        return date.toISOString();
    }

    /**
     * Get a cookie value by name.
     * @param {string} name - The cookie name.
     * @return {string|null} The cookie value or null.
     */
    function getCookie(name) {
        var match = document.cookie.match(new RegExp('(^| )' + name + '=([^;]+)'));
        return match ? match[2] : null;
    }

    /**
     * Set a cookie.
     * @param {string} name - The cookie name.
     * @param {string} value - The cookie value.
     * @param {number} days - Days until expiration.
     */
    function setCookie(name, value, days) {
        var expires = '';
        if (days) {
            var date = new Date();
            date.setTime(date.getTime() + (days * 24 * 60 * 60 * 1000));
            expires = '; expires=' + date.toUTCString();
        }
        document.cookie = name + '=' + value + expires + '; path=/';
    }

    return {
        formatDate: formatDate,
        getCookie: getCookie,
        setCookie: setCookie
    };
})();
