// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Private class fields, methods, statics and the `#field in object` brand
// check, mixed with public accessors.
class Counter {
  #count = 0;
  #name;
  static #max = 10;
  constructor(n) { this.#name = n; }
  get value() { return this.#count; }
  get name() { return this.#name; }
  increment() { this.#count += 1; return this; }
  #clamp() { return Math.min(this.#count, Counter.#max); }
  clamped() { return this.#clamp(); }
  static #parse(v) { return v | 0; }
  static build(v) {
    const c = new Counter('x');
    c.#count = Counter.#parse(v);
    return c;
  }
  static isCounter(o) { return #count in o; }
}
var c = Counter.build('7');
c.increment();
