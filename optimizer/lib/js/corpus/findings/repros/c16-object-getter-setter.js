// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Object-literal accessors, including property names that are themselves
// accessor or class keywords and so must not re-parse as one.
var cfg = {
  _port: 80,
  get port() { return this._port; },
  set port(v) { this._port = v | 0; },
  get ['host' + 'name']() { return 'localhost'; },
  set ['host' + 'name'](v) { this._host = v; },
  get get() { return 1; },
  set set(v) { this._setter = v; },
  get static() { return 2; },
  set async(v) { this._async = v; },
  get 0() { return 3; },
};
cfg.port = '8080';
var seen = cfg.port + cfg.get + cfg.set + cfg.static + cfg.async + cfg[0];
