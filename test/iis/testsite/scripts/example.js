// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Example JavaScript file for testing IPRO.
 * This file should be minified by PageSpeed.
 */

// Configuration object
var ExampleConfig = {
    debug: false,
    timeout: 5000,
    retries: 3,
    apiUrl: '/api/v1'
};

/**
 * Example class for demonstration.
 * @constructor
 */
function ExampleClass() {
    this.initialized = false;
    this.data = [];
}

/**
 * Initialize the example.
 * @return {boolean} Success status.
 */
ExampleClass.prototype.init = function() {
    if (this.initialized) {
        console.warn('Already initialized');
        return false;
    }

    console.log('Initializing ExampleClass...');
    this.initialized = true;
    return true;
};

/**
 * Add data to the collection.
 * @param {*} item - Item to add.
 */
ExampleClass.prototype.addData = function(item) {
    if (!this.initialized) {
        throw new Error('Not initialized');
    }
    this.data.push(item);
    console.log('Added item:', item);
};

/**
 * Get all data.
 * @return {Array} The data array.
 */
ExampleClass.prototype.getData = function() {
    return this.data.slice();
};

/**
 * Clear all data.
 */
ExampleClass.prototype.clearData = function() {
    this.data = [];
    console.log('Data cleared');
};

// Create global instance
var exampleInstance = new ExampleClass();

// Export for module systems
if (typeof module !== 'undefined' && module.exports) {
    module.exports = ExampleClass;
}
