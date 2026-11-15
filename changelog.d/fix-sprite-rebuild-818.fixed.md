- **Image sprites no longer answer 404 when an image is briefly unavailable.**
  Before, once a sprite was no longer cached and one of its images could not
  be fetched at that moment, the sprite URL answered 404 and the page lost all
  of its sprited images. The sprite is now rebuilt from the images up to the
  first one that is missing, so those keep the positions the page's styles
  use, and the rest show nothing instead of the wrong image. When the first
  image is the missing one, a transparent image is served. These answers have
  a short private lifetime, and the full sprite is served again once the
  image is back.
