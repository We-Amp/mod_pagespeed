/**
 * JavaScript file C for combine tests
 */

var AppC = (function() {
    'use strict';

    var listeners = {};

    function on(event, callback) {
        if (!listeners[event]) {
            listeners[event] = [];
        }
        listeners[event].push(callback);
        return function unsubscribe() {
            var index = listeners[event].indexOf(callback);
            if (index > -1) {
                listeners[event].splice(index, 1);
            }
        };
    }

    function emit(event, data) {
        var eventListeners = listeners[event];
        if (eventListeners) {
            eventListeners.forEach(function(callback) {
                try {
                    callback(data);
                } catch (e) {
                    console.error('Error in event listener:', e);
                }
            });
        }
    }

    function off(event) {
        if (event) {
            delete listeners[event];
        } else {
            listeners = {};
        }
    }

    return {
        on: on,
        emit: emit,
        off: off
    };
})();
