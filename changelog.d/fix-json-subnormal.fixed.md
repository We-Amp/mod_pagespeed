- **JSON-LD blocks that hold a very small number are no longer left unchanged on builds that use libc++,
  such as macOS builds.** A number that underflows to a subnormal value, such as `1e-320`, made the
  strict JSON reader refuse the whole block there, so the JSON-LD rule skipped it; such numbers are now
  read as on other builds.
