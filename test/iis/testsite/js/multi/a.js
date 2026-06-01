/**
 * JavaScript file A for combine tests
 */

var AppA = (function() {
    'use strict';

    var config = {
        version: '1.0.0',
        debug: false
    };

    function init() {
        console.log('AppA initialized');
        if (config.debug) {
            console.log('Debug mode enabled');
        }
    }

    function getVersion() {
        return config.version;
    }

    function setDebug(enabled) {
        config.debug = enabled;
    }

    return {
        init: init,
        getVersion: getVersion,
        setDebug: setDebug
    };
})();

// Initialize on DOM ready
document.addEventListener('DOMContentLoaded', AppA.init);
