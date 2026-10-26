- **nginx stores newly seen resources from a background thread when
  in-place optimization runs without the optimizer service.** Recording a
  cache miss no longer adds latency for other clients on the same worker.
  Under heavy load a recording may be skipped (counted in
  `ipro_classic_record_dropped`); the resource is recorded on a later
  request.
