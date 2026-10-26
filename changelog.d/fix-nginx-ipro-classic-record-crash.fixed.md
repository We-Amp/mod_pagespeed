- **nginx: in-place optimization of images and resources no longer stalls.**
  A worker could exit while finishing the recording of a newly seen
  resource without the optimizer service, which dropped the connections it
  was serving and kept the resource from being optimized.
