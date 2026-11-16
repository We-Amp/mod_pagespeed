- **Image sprites no longer answer 404 when an image is briefly unavailable.**
  Before, once a sprite was no longer cached and one of its images could not
  be fetched at that moment, the sprite URL answered 404. The sprite URL is
  now answered with a transparent image, so the page shows nothing in those
  places instead of the wrong image, with a short private lifetime. The full
  sprite is served again once the image is back.
