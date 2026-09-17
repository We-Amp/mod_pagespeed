Image Placeholders
==================

This directory should contain test images for the PageSpeed IIS test suite.
Copy images from install/mod_pagespeed_example/images/ for testing.

Required images for lazyload tests:
- Puzzle.jpg    - Main test image (256x192 pixels)
- Puzzle2.jpg   - Second puzzle image for lazyload testing

Additional recommended images:
- pagespeed_logo.png  - PageSpeed logo
- BikeCrashIcn.png    - PNG icon image
- Cuppa.png           - PNG cup image
- Beach.jpg           - JPEG photo
- IronChef2.gif       - Animated GIF
- CradleAnimation.gif - Animated GIF

Copy command (from repo root in PowerShell):
    Copy-Item install/mod_pagespeed_example/images/* test/iis/testsite/images/ -Force

Or generate test images with ImageMagick:
    convert -size 256x192 xc:blue -fill white -gravity center -annotate 0 "Puzzle" Puzzle.jpg
    convert -size 256x192 xc:green -fill white -gravity center -annotate 0 "Puzzle2" Puzzle2.jpg
    convert -size 200x50 xc:red -fill white -gravity center -annotate 0 "Logo" pagespeed_logo.png
