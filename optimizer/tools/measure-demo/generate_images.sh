#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Generate content-class-targeted test images for demo sites using ImageMagick.
#
# Four generator functions produce images that trigger specific content classes
# in the PageSpeed 2.0 content analyzer:
#   generate_photo       → kPhoto       (multi-channel fractal, wide gradient histogram)
#   generate_screenshot  → kScreenshot  (solid rectangles, hard edges, few colors)
#   generate_noisy       → kNoisy       (fractal + heavy Gaussian noise)
#   generate_illustration_svg_candidate → kIllustration (flat shapes, ≤16 colors, alpha)
#
# Output: $WEBSITE_DIR/public/demos/{site}/images/*.{jpg,png}
#
# WEBSITE_DIR names the modpagespeed.com site tree, which lives in the public
# repository (We-Amp/pagespeed-optimizer, website/). Default: <repo root>/website.
#
# Prerequisites: ImageMagick 7 (magick command)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
WEBSITE_DIR="${WEBSITE_DIR:-$REPO_ROOT/website}"
if [ ! -d "$WEBSITE_DIR" ]; then
    echo "Error: website tree not found at $WEBSITE_DIR" >&2
    echo "Set WEBSITE_DIR to the public repository's website/ checkout (We-Amp/pagespeed-optimizer)." >&2
    exit 1
fi
DEMOS_DIR="$WEBSITE_DIR/public/demos"

# Check for ImageMagick
if command -v magick &>/dev/null; then
    IM=magick
elif command -v convert &>/dev/null; then
    IM=convert
else
    echo "Error: ImageMagick not found. Install with: brew install imagemagick" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Generator: kPhoto
#
# Three independent plasma:fractal channels combined as RGB produce a wide
# gradient histogram → PhotoMetric >= 16.0.
# ---------------------------------------------------------------------------
generate_photo() {
    local output="$1"
    local width="$2"
    local height="$3"
    local seed="$4"
    local hue="${5:-100}"   # -modulate hue shift (100 = no shift)

    mkdir -p "$(dirname "$output")"

    # Three independent fractal-noise channels → wide color distribution
    $IM \( -size "${width}x${height}" -seed "$seed" plasma:fractal \
           -channel R -separate \) \
         \( -size "${width}x${height}" -seed "$((seed + 100))" plasma:fractal \
           -channel G -separate \) \
         \( -size "${width}x${height}" -seed "$((seed + 200))" plasma:fractal \
           -channel B -separate \) \
         -combine \
         -modulate 100,120,"$hue" \
         -unsharp 0x1 \
         -quality 92 "$output"
    echo "  Created [photo]: $(basename "$output") (${width}x${height})"
}

# ---------------------------------------------------------------------------
# Generator: kScreenshot
#
# Solid-color rectangles on a dark background with hard edges and minimal noise.
# Catppuccin-style palette → 8-10 flat colors, edge density >= 0.15.
# ---------------------------------------------------------------------------
generate_screenshot() {
    local output="$1"
    local width="$2"
    local height="$3"
    local seed="$4"

    mkdir -p "$(dirname "$output")"

    # Catppuccin-inspired colors for UI mockup look
    local bg="#1e1e2e"
    local colors=("#cdd6f4" "#89b4fa" "#a6e3a1" "#f38ba8" "#fab387"
                  "#f9e2af" "#94e2d5" "#b4befe" "#74c7ec" "#f5c2e7")

    # Start with dark background
    local cmd="$IM -size ${width}x${height} xc:${bg}"

    # Use seed to deterministically generate rectangles
    RANDOM=$seed
    local n_rects=$(( 8 + RANDOM % 5 ))  # 8-12 rectangles
    for (( i = 0; i < n_rects; i++ )); do
        local color="${colors[$((RANDOM % ${#colors[@]}))]}"
        local rx=$(( RANDOM % (width * 2 / 3) ))
        local ry=$(( RANDOM % (height * 2 / 3) ))
        local rw=$(( 40 + RANDOM % (width / 3) ))
        local rh=$(( 20 + RANDOM % (height / 3) ))
        cmd+=" -fill '${color}' -draw 'rectangle ${rx},${ry} $((rx + rw)),$((ry + rh))'"
    done

    # Add a thin horizontal line (like a nav bar separator)
    local line_y=$(( 30 + RANDOM % 20 ))
    cmd+=" -fill '#45475a' -draw 'rectangle 0,${line_y} ${width},$((line_y + 2))'"

    # Minimal noise to stay under the noise threshold
    cmd+=" -attenuate 0.02 +noise Gaussian"
    cmd+=" -quality 95 '${output}'"

    eval "$cmd"
    echo "  Created [screenshot]: $(basename "$output") (${width}x${height})"
}

# ---------------------------------------------------------------------------
# Generator: kNoisy
#
# Same fractal base as kPhoto with heavy Gaussian noise layered on top.
# MAD should exceed 15.0. Quality 95 preserves noise through JPEG compression.
# ---------------------------------------------------------------------------
generate_noisy() {
    local output="$1"
    local width="$2"
    local height="$3"
    local seed="$4"

    mkdir -p "$(dirname "$output")"

    # Fractal base + heavy noise
    $IM -size "${width}x${height}" -seed "$seed" plasma:fractal \
        -modulate 100,80,100 \
        -attenuate 0.25 +noise Gaussian \
        -quality 95 "$output"
    echo "  Created [noisy]: $(basename "$output") (${width}x${height})"
}

# ---------------------------------------------------------------------------
# Generator: kIllustration + SVG candidate
#
# Transparent canvas with solid-color geometric shapes (5-6 colors from a fixed
# palette). +antialias prevents extra colors from anti-aliasing. PNG32 output
# preserves alpha channel. Should score >= 50 on SVG candidacy.
# ---------------------------------------------------------------------------
generate_illustration_svg_candidate() {
    local output="$1"
    local width="$2"
    local height="$3"
    local seed="$4"

    mkdir -p "$(dirname "$output")"

    # Fixed palette of 6 flat colors (illustration style)
    local palette=("#4c6ef5" "#37b24d" "#f59f00" "#e64980" "#7048e8" "#1098ad")

    # Start with fully transparent canvas
    local cmd="$IM -size ${width}x${height} xc:none +antialias"

    # Add a large background shape (rounded rectangle covering ~80%)
    local bg_color="${palette[0]}"
    local margin_x=$(( width / 10 ))
    local margin_y=$(( height / 10 ))
    cmd+=" -fill '${bg_color}' -draw 'rectangle ${margin_x},${margin_y} $((width - margin_x)),$((height - margin_y))'"

    # Deterministic shapes using seed
    RANDOM=$seed
    local n_shapes=$(( 5 + RANDOM % 4 ))  # 5-8 shapes
    for (( i = 0; i < n_shapes; i++ )); do
        local color="${palette[$((1 + RANDOM % (${#palette[@]} - 1)))]}"
        local shape_type=$(( RANDOM % 3 ))

        if (( shape_type == 0 )); then
            # Circle
            local cx=$(( margin_x + RANDOM % (width - 2 * margin_x) ))
            local cy=$(( margin_y + RANDOM % (height - 2 * margin_y) ))
            local r=$(( 15 + RANDOM % 40 ))
            cmd+=" -fill '${color}' -draw 'circle ${cx},${cy} $((cx + r)),${cy}'"
        elif (( shape_type == 1 )); then
            # Rectangle
            local rx=$(( margin_x + RANDOM % (width / 2) ))
            local ry=$(( margin_y + RANDOM % (height / 2) ))
            local rw=$(( 20 + RANDOM % (width / 4) ))
            local rh=$(( 15 + RANDOM % (height / 4) ))
            cmd+=" -fill '${color}' -draw 'rectangle ${rx},${ry} $((rx + rw)),$((ry + rh))'"
        else
            # Triangle (polygon)
            local tx=$(( margin_x + RANDOM % (width / 2) ))
            local ty=$(( margin_y + RANDOM % (height / 2) ))
            local ts=$(( 20 + RANDOM % 40 ))
            cmd+=" -fill '${color}' -draw 'polygon ${tx},${ty} $((tx + ts)),$((ty + ts)) $((tx - ts / 2)),$((ty + ts))'"
        fi
    done

    cmd+=" PNG32:'${output}'"

    eval "$cmd"
    echo "  Created [illustration/svg-candidate]: $(basename "$output") (${width}x${height})"
}

echo "=== Generating demo site images ==="

# --- E-Commerce (6 product images, 400x300, JPEG) → kPhoto ---
echo ""
echo "E-Commerce Store (6 images, kPhoto):"
ECOM_DIR="$DEMOS_DIR/ecommerce/images"
generate_photo "$ECOM_DIR/headphones.jpg"   400 300 1001
generate_photo "$ECOM_DIR/tshirt.jpg"       400 300 1002 110
generate_photo "$ECOM_DIR/plantpot.jpg"     400 300 1003 130
generate_photo "$ECOM_DIR/powerbank.jpg"    400 300 1004 90
generate_photo "$ECOM_DIR/wallet.jpg"       400 300 1005 80
generate_photo "$ECOM_DIR/jacket.jpg"       400 300 1006 120

# --- Blog (5 article header images, 600x200, JPEG) → kScreenshot ---
echo ""
echo "Tech Blog (5 images, kScreenshot):"
BLOG_DIR="$DEMOS_DIR/blog/images"
generate_screenshot "$BLOG_DIR/ai-assistants.jpg"      600 200 2001
generate_screenshot "$BLOG_DIR/webassembly.jpg"        600 200 2002
generate_screenshot "$BLOG_DIR/rust-programming.jpg"   600 200 2003
generate_screenshot "$BLOG_DIR/edge-computing.jpg"     600 200 2004
generate_screenshot "$BLOG_DIR/container-security.jpg" 600 200 2005

# --- News Portal (10 images: mixed classes) ---
echo ""
echo "News Portal (10 images, mixed classes):"
NEWS_DIR="$DEMOS_DIR/news/images"
# kPhoto (hero + 4 cards)
generate_photo "$NEWS_DIR/quantum-hero.jpg"        800 400 3001
generate_photo "$NEWS_DIR/autonomous-vehicles.jpg" 300 200 3002
generate_photo "$NEWS_DIR/ocean-conservation.jpg"  300 200 3003 120
generate_photo "$NEWS_DIR/cancer-research.jpg"     300 200 3005 90
generate_photo "$NEWS_DIR/championship.jpg"        300 200 3006 110
# kNoisy
generate_noisy "$NEWS_DIR/central-banks.jpg"       300 200 3004
# kIllustration + SVG candidates (PNG with alpha)
generate_illustration_svg_candidate "$NEWS_DIR/space-telescope.png"  300 200 3007
generate_illustration_svg_candidate "$NEWS_DIR/infrastructure.png"   300 200 3008
generate_illustration_svg_candidate "$NEWS_DIR/open-source-ai.png"   300 200 3009
# kScreenshot
generate_screenshot "$NEWS_DIR/peace-agreement.jpg" 300 200 3010

# --- Portfolio (6 images: 5 kPhoto + 1 kNoisy) ---
echo ""
echo "Portfolio (6 images, kPhoto + kNoisy):"
PORT_DIR="$DEMOS_DIR/portfolio/images"
generate_photo "$PORT_DIR/hero-landscape.jpg"  1600 900 4001
generate_photo "$PORT_DIR/morning-mist.jpg"     800 600 4002 80
generate_photo "$PORT_DIR/ocean-whisper.jpg"     800 600 4003 130
generate_photo "$PORT_DIR/golden-hour.jpg"       800 600 4005 110
generate_photo "$PORT_DIR/still-waters.jpg"      800 600 4006 90
# kNoisy
generate_noisy "$PORT_DIR/forest-cathedral.jpg"  800 600 4004

echo ""
echo "=== Done: 27 images generated ==="
echo ""
echo "Content class distribution:"
echo "  kPhoto:        17 (ecommerce x6, news hero + 4 cards, portfolio x5)"
echo "  kScreenshot:    6 (blog x5, news x1)"
echo "  kIllustration:  3 (news x3, all SVG candidates, PNG with alpha)"
echo "  kNoisy:         2 (news x1, portfolio x1)"
echo ""
echo "Image directories:"
for site in ecommerce blog news portfolio; do
    jpg_count=$(find "$DEMOS_DIR/$site/images" -maxdepth 1 -name '*.jpg' 2>/dev/null | wc -l | tr -d ' ')
    png_count=$(find "$DEMOS_DIR/$site/images" -maxdepth 1 -name '*.png' 2>/dev/null | wc -l | tr -d ' ')
    size=$(du -sh "$DEMOS_DIR/$site/images/" 2>/dev/null | cut -f1)
    echo "  $site: ${jpg_count} JPEG + ${png_count} PNG ($size)"
done
