/**
 * JavaScript file B for combine tests
 */

var AppB = (function() {
    'use strict';

    var data = [];

    function addItem(item) {
        data.push(item);
        console.log('Added item:', item);
        return data.length;
    }

    function removeItem(index) {
        if (index >= 0 && index < data.length) {
            var removed = data.splice(index, 1);
            console.log('Removed item:', removed[0]);
            return removed[0];
        }
        return null;
    }

    function getItems() {
        return data.slice();
    }

    function clearItems() {
        data = [];
        console.log('All items cleared');
    }

    function getCount() {
        return data.length;
    }

    return {
        addItem: addItem,
        removeItem: removeItem,
        getItems: getItems,
        clearItems: clearItems,
        getCount: getCount
    };
})();
