- nginx: corrected a regression in a 2.5.0 hardening change that let a single
  oversized request briefly stall a worker. Affected: 2.5.0 to 2.6.0. Update
  recommended for nginx sites.
