- **IIS no longer falls back to a time-based key for beacon nonces.** When the
  system random number generator is unavailable, nonce generation now fails
  closed instead of using a key derived from the clock.
