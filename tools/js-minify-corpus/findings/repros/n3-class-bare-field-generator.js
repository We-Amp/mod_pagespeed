// Bare class field followed by a generator method: the linebreak after
// the field is ASI-load-bearing. Dropping it fuses `x` and `*gen()` into
// one invalid element (the `*` reads as a multiplication in initializer
// position). Static and computed-name variants corrupt the same way.
class C {
  x
  *gen() { yield 1; }
}
class D {
  static x
  *gen() { yield 2; }
}
class E {
  [a]
  *gen() { yield 3; }
}
