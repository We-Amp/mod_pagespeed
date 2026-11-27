// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Class accessors: prototype and static, computed names, and names that
// collide with accessor keywords.
class Temp {
  _scale = 'c';
  static _label = 'temp';
  get scale() { return this._scale; }
  set scale(s) { this._scale = s; }
  static get label() { return Temp._label; }
  static set label(v) { Temp._label = v; }
  get ['f' + 't']() { return this._scale === 'c'; }
  set ['f' + 't'](v) { this._scale = v ? 'c' : 'f'; }
  get get() { return 1; }
  set set(v) { this._setter = v; }
  static get static() { return 2; }
}
var t = new Temp();
t.scale = 'F';
