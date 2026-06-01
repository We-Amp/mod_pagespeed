Image Placeholders
==================

This directory should contain test images for the PageSpeed IIS test suite.
Add the following images:

- logo.png   - A small PNG logo (e.g., 200x50 pixels)
- photo.jpg  - A JPEG photo (e.g., 400x300 pixels)
- icon.gif   - A small GIF icon (e.g., 32x32 pixels)

For testing, you can use any appropriately sized images.
Alternatively, generate test images with ImageMagick:

    convert -size 200x50 xc:blue -fill white -gravity center -annotate 0 "Logo" logo.png
    convert -size 400x300 xc:green photo.jpg
    convert -size 32x32 xc:red icon.gif
