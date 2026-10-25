- IIS: native memory no longer grows with every response recorded for
  in-place optimization.
- IIS: the per-request context is freed on every early-exit path.
