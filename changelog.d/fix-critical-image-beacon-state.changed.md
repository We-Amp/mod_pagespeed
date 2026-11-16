- With `resize_rendered_image_dimensions` enabled, an image is now resized to
  its rendered size only once several recent beacon results agree on that
  size, instead of following the last result. Resizing starts after a few
  page views rather than the first one, and sizes stored by an earlier
  version are collected again.
