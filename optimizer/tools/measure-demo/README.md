# Demo Site Measurement Tool

Measures real optimization metrics from the PageSpeed 2.0 nginx + worker + cache
stack running against the demo sites.

## Prerequisites

- Docker with `pagespeed2-dev` image built (`./tools/docker-build.sh`)
- Python 3.12+
- Node.js (for Lighthouse, optional)
- ImageMagick (for image generation, optional if images exist)

## Quick Start

```bash
# Generate demo images (first time only)
./tools/measure-demo/generate_images.sh

# Run full measurement pipeline
./tools/measure-demo/run.sh

# Skip Lighthouse for faster results
./tools/measure-demo/run.sh --skip-lighthouse
```

## What It Does

1. Starts a Docker Compose stack (origin, worker, nginx) serving demo content
2. Fetches baseline sizes from the origin server
3. Triggers cache miss through nginx to start worker optimization
4. Polls for optimized variants (minified CSS/JS, WebP/AVIF images)
5. Optionally runs Lighthouse (3 runs each, median scores)
6. Validates results (CSS < original, WebP < JPEG, AVIF < WebP, etc.)
7. Writes `$WEBSITE_DIR/src/data/demo-metrics.json`

## The site tree

The demo sites and the metrics file live in the modpagespeed.com site tree,
which is in the public repository (We-Amp/pagespeed-optimizer, `website/`).
`WEBSITE_DIR` names that tree; it defaults to
`<repo root>/website`. Every tool here fails with a
clear message when the directory does not exist.

## Output

`$WEBSITE_DIR/src/data/demo-metrics.json` - imported by `demo.astro` to display
real optimization metrics on the website demo page; commit it in the public
repository.

## When to Re-Run

- After changing demo site content (HTML, CSS, JS, images)
- After changing worker optimization logic (minification, transcoding)
- After changing image quality settings
- Before releases to ensure metrics are current

## Files

| File | Purpose |
|------|---------|
| `generate_images.sh` | Generate JPEG test images with ImageMagick |
| `docker-compose.yml` | Measurement stack (origin, worker, nginx) |
| `nginx-measure.conf` | Nginx config for measurement stack |
| `measure.py` | Main measurement script |
| `run.sh` | Runner with prereq checks and stack management |

## Adding a New Demo Site

1. Create `$WEBSITE_DIR/public/demos/{site}/` with `index.html`, `style.css`, `app.js`
2. Generate images to `$WEBSITE_DIR/public/demos/{site}/images/`
3. Add the site definition to `SITES` in `measure.py`
4. Re-run `./tools/measure-demo/run.sh`
