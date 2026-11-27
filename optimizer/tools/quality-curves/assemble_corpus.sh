#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed — Corpus Assembly Script
#
# Assembles a 1500+ image corpus for quality curve training from multiple sources:
#   - DIV2K validation set (ETH Zurich)
#   - CC0 photos from picsum.photos and Lorem Flickr
#   - ImageMagick-generated screenshots, illustrations, and noisy images
#   - Existing test images from the repo
#
# Usage: assemble_corpus.sh [OUTPUT_DIR]
#   OUTPUT_DIR defaults to /tmp/quality-corpus/
#
# The script is idempotent — existing files are skipped.
# Requires: curl, ImageMagick (convert/magick), optionally Chrome/Chromium.

set -euo pipefail

CORPUS_DIR="${1:-/tmp/quality-corpus}"
JOBS="${JOBS:-8}"
RETRY_COUNT=3
RETRY_DELAY=2

# Content class directories.
PHOTO_DIR="$CORPUS_DIR/photo"
SCREENSHOT_DIR="$CORPUS_DIR/screenshot"
ILLUSTRATION_DIR="$CORPUS_DIR/illustration"
NOISY_DIR="$CORPUS_DIR/noisy"
UNKNOWN_DIR="$CORPUS_DIR/unknown"

mkdir -p "$PHOTO_DIR" "$SCREENSHOT_DIR" "$ILLUSTRATION_DIR" "$NOISY_DIR" "$UNKNOWN_DIR"

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Download with retries. Skips if output file already exists.
download() {
  local url="$1" output="$2"
  if [[ -f "$output" ]]; then return 0; fi
  local attempt
  for attempt in $(seq 1 "$RETRY_COUNT"); do
    if curl -fsSL --connect-timeout 10 --max-time 60 -o "$output" "$url" 2>/dev/null; then
      # Verify it's not empty or an error page.
      local size
      size=$(stat -c%s "$output" 2>/dev/null || stat -f%z "$output" 2>/dev/null || echo 0)
      if [[ "$size" -gt 1000 ]]; then
        return 0
      fi
      rm -f "$output"
    fi
    sleep "$RETRY_DELAY"
  done
  rm -f "$output"
  return 1
}

# Check for ImageMagick.
CONVERT_CMD=""
if command -v magick &>/dev/null; then
  CONVERT_CMD="magick"
elif command -v convert &>/dev/null; then
  CONVERT_CMD="convert"
else
  log "WARNING: ImageMagick not found. Synthetic images will be skipped."
fi

# ============================================================================
# PHASE 1: Photos (~600 images)
# ============================================================================
log "=== Phase 1: Photos ==="

# 1a. DIV2K validation set (100 images, 2K resolution, CC0).
DIV2K_DIR="$PHOTO_DIR/div2k"
mkdir -p "$DIV2K_DIR"
DIV2K_URL="https://data.vision.ee.ethz.ch/cvl/DIV2K/DIV2K_valid_HR.zip"
DIV2K_ZIP="$CORPUS_DIR/.div2k_valid.zip"

if [[ ! -f "$DIV2K_DIR/.done" ]]; then
  log "Downloading DIV2K validation set..."
  if download "$DIV2K_URL" "$DIV2K_ZIP"; then
    unzip -q -j -o "$DIV2K_ZIP" -d "$DIV2K_DIR" 2>/dev/null || true
    touch "$DIV2K_DIR/.done"
    rm -f "$DIV2K_ZIP"
    log "DIV2K: $(find "$DIV2K_DIR" -name '*.png' | wc -l) images"
  else
    log "WARNING: DIV2K download failed. Continuing without it."
  fi
else
  log "DIV2K already downloaded."
fi

# 1b. Picsum.photos CC0 photos at varied sizes (200 images).
PICSUM_DIR="$PHOTO_DIR/picsum"
mkdir -p "$PICSUM_DIR"
log "Downloading picsum.photos CC0 images..."
picsum_count=0
for i in $(seq 1 250); do
  # Vary dimensions: 300-2000px.
  w=$(( 300 + (i * 7) % 1700 ))
  h=$(( 300 + (i * 11) % 1400 ))
  outfile="$PICSUM_DIR/picsum_${i}_${w}x${h}.jpg"
  if [[ -f "$outfile" ]]; then
    picsum_count=$((picsum_count + 1))
    continue
  fi
  if download "https://picsum.photos/${w}/${h}.jpg" "$outfile"; then
    picsum_count=$((picsum_count + 1))
  fi
  # Rate limit: picsum.photos is free, be polite.
  sleep 0.3
done &
PICSUM_PID=$!
log "Picsum download running in background (PID $PICSUM_PID)..."

# 1c. Lorem Flickr CC0 photos (150 images, different categories).
FLICKR_DIR="$PHOTO_DIR/flickr"
mkdir -p "$FLICKR_DIR"
log "Downloading Lorem Flickr CC0 images..."
CATEGORIES=("nature" "city" "food" "people" "animals" "architecture" "sports" "travel" "technology" "abstract")
flickr_count=0
for i in $(seq 1 150); do
  cat_idx=$(( (i - 1) % ${#CATEGORIES[@]} ))
  category="${CATEGORIES[$cat_idx]}"
  w=$(( 400 + (i * 13) % 1200 ))
  h=$(( 400 + (i * 17) % 1000 ))
  outfile="$FLICKR_DIR/flickr_${category}_${i}_${w}x${h}.jpg"
  if [[ -f "$outfile" ]]; then
    flickr_count=$((flickr_count + 1))
    continue
  fi
  if download "https://loremflickr.com/${w}/${h}/${category}" "$outfile"; then
    flickr_count=$((flickr_count + 1))
  fi
  sleep 0.4
done &
FLICKR_PID=$!
log "Lorem Flickr download running in background (PID $FLICKR_PID)..."

# 1d. Pexels-style varied photos via picsum with different seeds (100 more).
VARIED_DIR="$PHOTO_DIR/varied"
mkdir -p "$VARIED_DIR"
log "Downloading varied CC0 photos..."
for i in $(seq 251 350); do
  w=$(( 500 + (i * 3) % 1500 ))
  h=$(( 400 + (i * 5) % 1200 ))
  outfile="$VARIED_DIR/varied_${i}_${w}x${h}.jpg"
  if [[ -f "$outfile" ]]; then continue; fi
  download "https://picsum.photos/seed/${i}/${w}/${h}.jpg" "$outfile" || true
  sleep 0.3
done &
VARIED_PID=$!

# ============================================================================
# PHASE 2: Screenshots (~300 images, ImageMagick-generated)
# ============================================================================
log "=== Phase 2: Screenshots ==="

if [[ -n "$CONVERT_CMD" ]]; then
  # Generate synthetic UI mockups with rectangles, text, and web-like patterns.
  # Catppuccin Mocha palette colors.
  BG_COLORS=("#1e1e2e" "#313244" "#45475a" "#f5e0dc" "#cdd6f4" "#f5f5f5" "#ffffff" "#e6e9ef")
  FG_COLORS=("#cdd6f4" "#f38ba8" "#a6e3a1" "#89b4fa" "#fab387" "#1e1e2e" "#313244" "#585b70")
  ACCENT_COLORS=("#f38ba8" "#89b4fa" "#a6e3a1" "#f9e2af" "#cba6f7" "#94e2d5" "#74c7ec" "#f5c2e7")

  screenshot_count=0
  for i in $(seq 1 300); do
    outfile="$SCREENSHOT_DIR/screenshot_${i}.png"
    if [[ -f "$outfile" ]]; then
      screenshot_count=$((screenshot_count + 1))
      continue
    fi

    # Vary dimensions to simulate different viewport sizes.
    case $(( i % 5 )) in
      0) w=1920; h=1080;;  # Desktop full
      1) w=1440; h=900;;   # Desktop compact
      2) w=768; h=1024;;   # Tablet portrait
      3) w=375; h=667;;    # Mobile
      4) w=1280; h=720;;   # Laptop
    esac

    bg="${BG_COLORS[$(( i % ${#BG_COLORS[@]} ))]}"
    fg="${FG_COLORS[$(( i % ${#FG_COLORS[@]} ))]}"
    accent="${ACCENT_COLORS[$(( i % ${#ACCENT_COLORS[@]} ))]}"

    # Create base with background color.
    $CONVERT_CMD -size "${w}x${h}" "xc:${bg}" \
      -fill "$fg" -draw "rectangle 0,0 ${w},50" \
      -fill "$accent" -draw "rectangle 10,60 $((w/3)),200" \
      -fill "$fg" -draw "rectangle $((w/3+20)),60 $((2*w/3)),200" \
      -fill "$accent" -draw "rectangle $((2*w/3+20)),60 $((w-10)),200" \
      -fill "$fg" -pointsize 14 -annotate +20+35 "Navigation Bar - Menu Item 1 | Item 2 | Item 3" \
      -fill "$fg" -draw "rectangle 10,220 $((w-10)),$((h-20))" \
      -fill "$bg" -draw "rectangle 15,225 $((w-15)),$((h-25))" \
      -fill "$fg" -pointsize 12 -annotate +30+250 "Lorem ipsum dolor sit amet, consectetur adipiscing elit." \
      -fill "$accent" -draw "circle $((w/2)),$((h/2)) $((w/2+30)),$((h/2))" \
      "$outfile" 2>/dev/null || true

    screenshot_count=$((screenshot_count + 1))
  done
  log "Screenshots: $screenshot_count generated"
else
  log "Skipping screenshot generation (no ImageMagick)."
fi

# ============================================================================
# PHASE 3: Illustrations (~200 images, ImageMagick-generated)
# ============================================================================
log "=== Phase 3: Illustrations ==="

if [[ -n "$CONVERT_CMD" ]]; then
  illustration_count=0
  for i in $(seq 1 200); do
    outfile="$ILLUSTRATION_DIR/illustration_${i}.png"
    if [[ -f "$outfile" ]]; then
      illustration_count=$((illustration_count + 1))
      continue
    fi

    # Vary sizes: icons (64-256px) and larger illustrations (400-1200px).
    if (( i <= 80 )); then
      # Icons / small illustrations.
      sz=$(( 64 + (i * 3) % 192 ))
      w=$sz; h=$sz
    else
      # Larger illustrations / charts / diagrams.
      w=$(( 400 + (i * 7) % 800 ))
      h=$(( 300 + (i * 11) % 600 ))
    fi

    # Flat-color geometric patterns.
    colors=("#f38ba8" "#a6e3a1" "#89b4fa" "#f9e2af" "#cba6f7" "#94e2d5" "#fab387" "#f5c2e7")
    c1="${colors[$(( i % ${#colors[@]} ))]}"
    c2="${colors[$(( (i + 3) % ${#colors[@]} ))]}"
    c3="${colors[$(( (i + 5) % ${#colors[@]} ))]}"

    case $(( i % 6 )) in
      0)
        # Gradient background with geometric shapes.
        $CONVERT_CMD -size "${w}x${h}" "gradient:${c1}-${c2}" \
          -fill "$c3" -draw "rectangle $((w/4)),$((h/4)) $((3*w/4)),$((3*h/4))" \
          "$outfile" 2>/dev/null || true
        ;;
      1)
        # Concentric circles (icon-like).
        $CONVERT_CMD -size "${w}x${h}" "xc:${c1}" \
          -fill "$c2" -draw "circle $((w/2)),$((h/2)) $((w/2+w/3)),$((h/2))" \
          -fill "$c3" -draw "circle $((w/2)),$((h/2)) $((w/2+w/6)),$((h/2))" \
          "$outfile" 2>/dev/null || true
        ;;
      2)
        # Horizontal bars (chart-like).
        $CONVERT_CMD -size "${w}x${h}" "xc:white" \
          -fill "$c1" -draw "rectangle 10,$((h/8)) $((w*3/4)),$((h/4))" \
          -fill "$c2" -draw "rectangle 10,$((h*3/8)) $((w/2)),$((h/2))" \
          -fill "$c3" -draw "rectangle 10,$((h*5/8)) $((w*2/3)),$((h*3/4))" \
          "$outfile" 2>/dev/null || true
        ;;
      3)
        # Diagonal split.
        $CONVERT_CMD -size "${w}x${h}" "xc:${c1}" \
          -fill "$c2" -draw "polygon 0,0 ${w},0 ${w},${h}" \
          -fill "$c3" -draw "circle $((w/2)),$((h/2)) $((w/2+w/8)),$((h/2))" \
          "$outfile" 2>/dev/null || true
        ;;
      4)
        # Grid pattern.
        $CONVERT_CMD -size "${w}x${h}" "xc:${c1}" \
          -fill "$c2" -draw "rectangle 0,0 $((w/2-2)),$((h/2-2))" \
          -fill "$c3" -draw "rectangle $((w/2+2)),0 ${w},$((h/2-2))" \
          -fill "$c3" -draw "rectangle 0,$((h/2+2)) $((w/2-2)),${h}" \
          -fill "$c2" -draw "rectangle $((w/2+2)),$((h/2+2)) ${w},${h}" \
          "$outfile" 2>/dev/null || true
        ;;
      5)
        # Radial gradient with border.
        $CONVERT_CMD -size "${w}x${h}" "radial-gradient:${c1}-${c2}" \
          -bordercolor "$c3" -border 4 \
          -resize "${w}x${h}!" \
          "$outfile" 2>/dev/null || true
        ;;
    esac

    illustration_count=$((illustration_count + 1))
  done
  log "Illustrations: $illustration_count generated"
else
  log "Skipping illustration generation (no ImageMagick)."
fi

# ============================================================================
# PHASE 4: Noisy images (~200 images)
# ============================================================================
log "=== Phase 4: Noisy images ==="

if [[ -n "$CONVERT_CMD" ]]; then
  noisy_count=0

  # 4a. Generate photos with Gaussian noise added (100 images).
  # Download base photos, then add noise.
  NOISY_BASE_DIR="$NOISY_DIR/.base"
  mkdir -p "$NOISY_BASE_DIR"

  for i in $(seq 1 100); do
    outfile="$NOISY_DIR/noisy_gaussian_${i}.jpg"
    if [[ -f "$outfile" ]]; then
      noisy_count=$((noisy_count + 1))
      continue
    fi

    basefile="$NOISY_BASE_DIR/base_${i}.jpg"
    w=$(( 400 + (i * 13) % 800 ))
    h=$(( 300 + (i * 7) % 600 ))

    # Download a base photo if needed.
    if [[ ! -f "$basefile" ]]; then
      download "https://picsum.photos/seed/noisy${i}/${w}/${h}.jpg" "$basefile" || continue
      sleep 0.2
    fi

    # Add varying amounts of Gaussian noise.
    sigma=$(( 5 + (i % 30) ))
    $CONVERT_CMD "$basefile" -attenuate "$sigma" +noise Gaussian "$outfile" 2>/dev/null || true
    noisy_count=$((noisy_count + 1))
  done

  # 4b. Heavy JPEG recompression artifacts (50 images).
  for i in $(seq 1 50); do
    outfile="$NOISY_DIR/noisy_recompressed_${i}.jpg"
    if [[ -f "$outfile" ]]; then
      noisy_count=$((noisy_count + 1))
      continue
    fi

    basefile="$NOISY_BASE_DIR/base_$((i + 100)).jpg"
    w=$(( 400 + (i * 11) % 800 ))
    h=$(( 300 + (i * 9) % 600 ))

    if [[ ! -f "$basefile" ]]; then
      download "https://picsum.photos/seed/recomp${i}/${w}/${h}.jpg" "$basefile" || continue
      sleep 0.2
    fi

    # Re-save at very low quality (30-50) to introduce compression artifacts.
    q=$(( 30 + (i % 20) ))
    $CONVERT_CMD "$basefile" -quality "$q" "$outfile" 2>/dev/null || true
    noisy_count=$((noisy_count + 1))
  done

  # 4c. Low-contrast / dark images (50 images).
  for i in $(seq 1 50); do
    outfile="$NOISY_DIR/noisy_dark_${i}.jpg"
    if [[ -f "$outfile" ]]; then
      noisy_count=$((noisy_count + 1))
      continue
    fi

    basefile="$NOISY_BASE_DIR/base_$((i + 150)).jpg"
    w=$(( 400 + (i * 7) % 800 ))
    h=$(( 300 + (i * 5) % 600 ))

    if [[ ! -f "$basefile" ]]; then
      download "https://picsum.photos/seed/dark${i}/${w}/${h}.jpg" "$basefile" || continue
      sleep 0.2
    fi

    # Darken and reduce contrast.
    $CONVERT_CMD "$basefile" -brightness-contrast "-30x-40" \
      -attenuate 3 +noise Gaussian "$outfile" 2>/dev/null || true
    noisy_count=$((noisy_count + 1))
  done

  log "Noisy: $noisy_count generated"
else
  log "Skipping noisy image generation (no ImageMagick)."
fi

# ============================================================================
# PHASE 5: Unknown/Mixed (~200 images, ImageMagick composites)
# ============================================================================
log "=== Phase 5: Unknown/Mixed images ==="

if [[ -n "$CONVERT_CMD" ]]; then
  unknown_count=0

  for i in $(seq 1 200); do
    outfile="$UNKNOWN_DIR/unknown_${i}.png"
    if [[ -f "$outfile" ]]; then
      unknown_count=$((unknown_count + 1))
      continue
    fi

    w=$(( 300 + (i * 11) % 1000 ))
    h=$(( 300 + (i * 7) % 800 ))

    case $(( i % 5 )) in
      0)
        # Text overlay on gradient (meme-style).
        $CONVERT_CMD -size "${w}x${h}" "gradient:#1e1e2e-#313244" \
          -fill white -pointsize 28 -gravity center \
          -annotate +0+0 "Sample Text Overlay\nLine Two Here" \
          "$outfile" 2>/dev/null || true
        ;;
      1)
        # Mixed content: half photo-gradient, half flat color.
        $CONVERT_CMD -size "${w}x${h}" "plasma:${w}x${h}" \
          -fill "#f5f5f5" -draw "rectangle 0,0 $((w/2)),${h}" \
          -fill "#1e1e2e" -pointsize 16 -annotate +20+30 "Data Table\nRow 1: Value\nRow 2: Value" \
          "$outfile" 2>/dev/null || true
        ;;
      2)
        # Infographic-style: colored blocks with text.
        $CONVERT_CMD -size "${w}x${h}" "xc:white" \
          -fill "#89b4fa" -draw "rectangle 10,10 $((w/3)),100" \
          -fill "#a6e3a1" -draw "rectangle $((w/3+5)),10 $((2*w/3)),100" \
          -fill "#f38ba8" -draw "rectangle $((2*w/3+5)),10 $((w-10)),100" \
          -fill "#1e1e2e" -pointsize 14 -annotate +15+140 "Statistics and Data Visualization" \
          -fill "#45475a" -draw "line 10,$((h/2)) $((w-10)),$((h/2))" \
          "$outfile" 2>/dev/null || true
        ;;
      3)
        # Scanned document style: slightly rotated text on off-white.
        $CONVERT_CMD -size "${w}x${h}" "xc:#f5f0e0" \
          -fill "#333333" -pointsize 11 \
          -annotate +20+30 "Document Title\n\nLorem ipsum dolor sit amet, consectetur\nadipiscing elit. Sed do eiusmod tempor\nincididunt ut labore et dolore magna aliqua." \
          -rotate 0.5 -crop "${w}x${h}+0+0!" \
          "$outfile" 2>/dev/null || true
        ;;
      4)
        # Comic panel style: bold outlines on colors.
        $CONVERT_CMD -size "${w}x${h}" "xc:#f9e2af" \
          -stroke black -strokewidth 3 -fill "none" \
          -draw "rectangle $((w/4)),$((h/4)) $((3*w/4)),$((3*h/4))" \
          -draw "line 0,0 $((w/4)),$((h/4))" \
          -draw "line ${w},0 $((3*w/4)),$((h/4))" \
          -fill "#1e1e2e" -stroke none -pointsize 20 \
          -annotate +$((w/3)),$((h/2)) "POW!" \
          "$outfile" 2>/dev/null || true
        ;;
    esac

    unknown_count=$((unknown_count + 1))
  done
  log "Unknown/Mixed: $unknown_count generated"
else
  log "Skipping unknown/mixed generation (no ImageMagick)."
fi

# ============================================================================
# PHASE 6: Copy suitable test images from repo
# ============================================================================
log "=== Phase 6: Copying test images ==="

TESTDATA_DIR="$(dirname "$0")/../../test/lib/image/testdata"
if [[ -d "$TESTDATA_DIR" ]]; then
  TESTCOPY_DIR="$CORPUS_DIR/testdata"
  mkdir -p "$TESTCOPY_DIR"
  testdata_count=0

  while IFS= read -r -d '' img; do
    filename=$(basename "$img")
    outfile="$TESTCOPY_DIR/$filename"
    if [[ -f "$outfile" ]]; then
      testdata_count=$((testdata_count + 1))
      continue
    fi

    # Skip tiny images (<64px in either dimension) and animated GIFs.
    # Quick size check via identify if available.
    if command -v identify &>/dev/null; then
      dims=$(identify -format "%w %h" "$img" 2>/dev/null | head -1) || continue
      read -r iw ih <<< "$dims"
      if [[ -n "$iw" && -n "$ih" ]] && (( iw < 64 || ih < 64 )); then
        continue
      fi
    fi

    cp "$img" "$outfile" 2>/dev/null || true
    testdata_count=$((testdata_count + 1))
  done < <(find "$TESTDATA_DIR" -maxdepth 1 \( -name '*.jpg' -o -name '*.png' -o -name '*.webp' -o -name '*.gif' \) -print0 2>/dev/null)

  log "Test images copied: $testdata_count"
else
  log "WARNING: Test data directory not found at $TESTDATA_DIR"
fi

# ============================================================================
# Wait for background downloads
# ============================================================================
log "Waiting for background downloads to complete..."
wait "$PICSUM_PID" 2>/dev/null || true
wait "$FLICKR_PID" 2>/dev/null || true
wait "$VARIED_PID" 2>/dev/null || true

# ============================================================================
# Summary
# ============================================================================
log "=== Corpus Assembly Complete ==="
total=0
for class_dir in "$PHOTO_DIR" "$SCREENSHOT_DIR" "$ILLUSTRATION_DIR" "$NOISY_DIR" "$UNKNOWN_DIR"; do
  class_name=$(basename "$class_dir")
  count=$(find "$class_dir" -type f \( -name '*.jpg' -o -name '*.png' -o -name '*.webp' -o -name '*.gif' \) | wc -l)
  log "  $class_name: $count images"
  total=$((total + count))
done
if [[ -d "$CORPUS_DIR/testdata" ]]; then
  td_count=$(find "$CORPUS_DIR/testdata" -type f | wc -l)
  log "  testdata: $td_count images"
  total=$((total + td_count))
fi
log "  TOTAL: $total images in $CORPUS_DIR"
