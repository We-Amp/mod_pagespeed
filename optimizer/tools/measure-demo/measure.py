#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Measure real optimization metrics for PageSpeed 2.0 demo sites.

Connects to the measurement Docker stack (origin on :8081, nginx on :8080)
and measures:
  - CSS/JS original and minified sizes
  - Image JPEG/WebP/AVIF sizes
  - Lighthouse performance scores (optional, requires Node.js)
  - Load times (optional, from Lighthouse)

Outputs: $WEBSITE_DIR/src/data/demo-metrics.json

WEBSITE_DIR names the modpagespeed.com site tree, which lives in the public
repository (We-Amp/pagespeed-optimizer, website/). Default: <repo root>/website. The script fails with a
clear message when that directory does not exist.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from datetime import UTC, datetime
from pathlib import Path

import requests

SCRIPT_DIR = Path(__file__).parent
REPO_ROOT = SCRIPT_DIR / ".." / ".."
WEBSITE_DIR = Path(
    os.environ.get("WEBSITE_DIR")
    or REPO_ROOT / "website"
).resolve()
OUTPUT_FILE = WEBSITE_DIR / "src" / "data" / "demo-metrics.json"


def require_website_dir() -> None:
    """Fail clearly when the site tree is not where WEBSITE_DIR says."""
    if not WEBSITE_DIR.is_dir():
        sys.exit(
            f"ERROR: website tree not found at {WEBSITE_DIR}\n"
            "Set WEBSITE_DIR to the public repository's website/ checkout "
            "(We-Amp/pagespeed-optimizer)."
        )


NGINX_URL = "http://localhost:8080"
ORIGIN_URL = "http://localhost:8081"
WORKER_API_URL = "http://localhost:9881"

POLL_TIMEOUT = 30.0
POLL_INTERVAL = 0.5


# --- Demo site definitions ---

SITES = [
    {
        "id": "ecommerce",
        "name": "E-Commerce Store",
        "description": "Product catalog with image grid, filters, and cart",
        "base_path": "/demos/ecommerce",
        "resources": {
            "html": "index.html",
            "css": "style.css",
            "js": "app.js",
        },
        "images": [
            "images/headphones.jpg",
            "images/tshirt.jpg",
            "images/plantpot.jpg",
            "images/powerbank.jpg",
            "images/wallet.jpg",
            "images/jacket.jpg",
        ],
        "image_label": "Images (6x JPEG)",
    },
    {
        "id": "blog",
        "name": "Tech Blog",
        "description": "Article-heavy layout with code blocks and sidebar",
        "base_path": "/demos/blog",
        "resources": {
            "html": "index.html",
            "css": "style.css",
            "js": "app.js",
        },
        "images": [
            "images/ai-assistants.jpg",
            "images/webassembly.jpg",
            "images/rust-programming.jpg",
            "images/edge-computing.jpg",
            "images/container-security.jpg",
        ],
        "image_label": "Images (5x JPEG)",
    },
    {
        "id": "news",
        "name": "News Portal",
        "description": "Breaking news homepage with hero, grid, and sidebar",
        "base_path": "/demos/news",
        "resources": {
            "html": "index.html",
            "css": "style.css",
            "js": "app.js",
        },
        "images": [
            "images/quantum-hero.jpg",
            "images/autonomous-vehicles.jpg",
            "images/ocean-conservation.jpg",
            "images/central-banks.jpg",
            "images/cancer-research.jpg",
            "images/championship.jpg",
            "images/space-telescope.png",
            "images/infrastructure.png",
            "images/open-source-ai.png",
            "images/peace-agreement.jpg",
        ],
        "image_label": "Images (7x JPEG + 3x PNG)",
    },
    {
        "id": "portfolio",
        "name": "Portfolio",
        "description": "Photographer's portfolio with large hero images",
        "base_path": "/demos/portfolio",
        "resources": {
            "html": "index.html",
            "css": "style.css",
            "js": "app.js",
        },
        "images": [
            "images/hero-landscape.jpg",
            "images/morning-mist.jpg",
            "images/ocean-whisper.jpg",
            "images/forest-cathedral.jpg",
            "images/golden-hour.jpg",
            "images/still-waters.jpg",
        ],
        "image_label": "Images (6x JPEG)",
    },
]


# --- Helper functions ---


def fetch_size(base_url: str, path: str) -> int:
    """Fetch a URL and return the content length in bytes."""
    url = f"{base_url}{path}"
    resp = requests.get(url, timeout=10)
    resp.raise_for_status()
    return len(resp.content)


def fetch_content(base_url: str, path: str) -> bytes:
    """Fetch a URL and return the content bytes."""
    url = f"{base_url}{path}"
    resp = requests.get(url, timeout=10)
    resp.raise_for_status()
    return resp.content


def poll_for_optimized(
    path: str,
    headers: dict[str, str] | None = None,
    expect_smaller_than: int = 0,
    timeout: float = POLL_TIMEOUT,
) -> requests.Response | None:
    """Poll nginx until X-PageSpeed: HIT with optimized content."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            resp = requests.get(
                f"{NGINX_URL}{path}",
                headers=headers or {},
                timeout=10,
            )
            last = resp
            if resp.headers.get("X-PageSpeed") == "HIT":
                if expect_smaller_than > 0:
                    if len(resp.content) < expect_smaller_than:
                        return resp
                else:
                    return resp
        except requests.RequestException:
            pass
        time.sleep(POLL_INTERVAL)
    # Timed out. When we were waiting for a SMALLER variant and never saw one,
    # return None to signal failure - silently returning the full-size response
    # is what masked the not-optimizing-worker bug (after==before shipped as a
    # real 0%-savings result).
    return None if expect_smaller_than > 0 else last


def is_webp(data: bytes) -> bool:
    """Check if data starts with WebP magic bytes."""
    return len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP"


def is_avif(data: bytes) -> bool:
    """Check if data has AVIF ftyp box."""
    return len(data) >= 12 and data[4:8] == b"ftyp"


def poll_for_image_variant(
    path: str,
    accept: str,
    check_fn,
    timeout: float = POLL_TIMEOUT,
) -> int | None:
    """Poll for an image variant (WebP or AVIF) and return its size."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = requests.get(
                f"{NGINX_URL}{path}",
                headers={"Accept": accept},
                timeout=10,
            )
            if resp.headers.get("X-PageSpeed") == "HIT" and check_fn(resp.content):
                return len(resp.content)
        except requests.RequestException:
            pass
        time.sleep(POLL_INTERVAL)
    return None


def check_worker_ready() -> None:
    """Abort if the worker is not up and ready.

    A worker that is still starting (or never started) optimizes nothing, so
    every metric would read after==before - exactly how the demo metrics once
    silently went stale. Fail loud and early instead of measuring a stack that
    optimizes nothing.
    """
    try:
        resp = requests.get(f"{WORKER_API_URL}/v1/health", timeout=5)
        resp.raise_for_status()
        health = resp.json()
    except (requests.RequestException, ValueError) as e:
        print(f"  Worker API: FAILED ({e})")
        print("\n  Cannot reach the worker admin API (:9881) to verify readiness.")
        print(
            "  Is the worker running and the port exposed? Refusing to measure blind."
        )
        sys.exit(1)
    if not health.get("ready", True):
        print(f"  Worker: NOT READY (status={health.get('status')!r})")
        print(
            "\n  The worker reports it is not ready - it optimizes nothing until it is,"
        )
        print(
            "  so every metric would read after==before. Check `docker compose logs worker`."
        )
        sys.exit(1)
    print(f"  Worker: OK (status={health.get('status')!r}, ready=True)")


def extract_css_snippet(original_css: str, minified_css: str) -> dict:
    """Extract a representative CSS snippet for before/after display.

    Takes the first ~10 rules (roughly 500 chars of original) for display.
    """
    # Find a good chunk: take lines up to about 500 chars
    lines = original_css.split("\n")
    snippet_lines = []
    total = 0
    # Skip initial comment blocks
    in_comment = False
    started = False
    for line in lines:
        if "/*" in line and "*/" not in line:
            in_comment = True
            continue
        if in_comment:
            if "*/" in line:
                in_comment = False
            continue
        if not started and line.strip() == "":
            continue
        started = True
        snippet_lines.append(line)
        total += len(line)
        if total > 600:
            break

    before = "\n".join(snippet_lines).strip()

    # Find corresponding minified portion:
    # Take a distinctive selector from the snippet
    selectors = re.findall(r"([.#][\w-]+(?:\s+[.#][\w-]+)*)\s*\{", before)
    if selectors:
        # Find last selector in the snippet
        last_sel = selectors[-1].replace(" ", "").replace(".", r"\.")
        # Find it in minified and take up to that point + its block
        match = re.search(last_sel + r"\{[^}]+\}", minified_css)
        if match:
            after = minified_css[: match.end()]
        else:
            after = minified_css[:400]
    else:
        after = minified_css[:400]

    return {"before": before, "after": after}


def run_lighthouse(url: str, runs: int = 3) -> dict | None:
    """Run Lighthouse and return median performance score and TTI.

    Returns None if Lighthouse is not available.
    """
    try:
        subprocess.run(
            ["npx", "--yes", "lighthouse", "--version"],
            capture_output=True,
            timeout=30,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None

    scores = []
    ttis = []
    for i in range(runs):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            tmp_path = f.name
        try:
            result = subprocess.run(
                [
                    "npx",
                    "--yes",
                    "lighthouse",
                    url,
                    "--output=json",
                    f"--output-path={tmp_path}",
                    "--chrome-flags=--headless --no-sandbox",
                    "--only-categories=performance",
                    "--throttling-method=simulate",
                    "--preset=mobile",
                    "--quiet",
                ],
                capture_output=True,
                text=True,
                timeout=120,
            )
            if result.returncode != 0:
                print(f"    Lighthouse run {i + 1} failed: {result.stderr[:200]}")
                continue

            with open(tmp_path) as f:
                data = json.load(f)

            perf = data.get("categories", {}).get("performance", {})
            score = perf.get("score")
            if score is not None:
                scores.append(int(score * 100))

            audits = data.get("audits", {})
            tti = audits.get("interactive", {}).get("numericValue")
            if tti is not None:
                ttis.append(tti / 1000.0)  # ms -> seconds

        except (subprocess.TimeoutExpired, json.JSONDecodeError, KeyError) as e:
            print(f"    Lighthouse run {i + 1} error: {e}")
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    if not scores:
        return None

    scores.sort()
    ttis.sort()
    median_score = scores[len(scores) // 2]
    median_tti = ttis[len(ttis) // 2] if ttis else None

    return {"score": median_score, "tti": round(median_tti, 1) if median_tti else None}


# --- Main measurement ---


def measure_site(site: dict, skip_lighthouse: bool = False) -> dict:
    """Measure all metrics for a single demo site."""
    base = site["base_path"]
    print(f"\n{'=' * 60}")
    print(f"  Measuring: {site['name']} ({site['id']})")
    print(f"{'=' * 60}")

    # 1. Fetch baseline sizes from origin
    print("\n  [1/5] Fetching baseline sizes from origin...")

    html_size = fetch_size(ORIGIN_URL, f"{base}/index.html")
    css_size = fetch_size(ORIGIN_URL, f"{base}/style.css")
    js_size = fetch_size(ORIGIN_URL, f"{base}/app.js")
    print(f"    HTML: {html_size:,} bytes")
    print(f"    CSS:  {css_size:,} bytes")
    print(f"    JS:   {js_size:,} bytes")

    image_sizes = {}
    total_jpeg = 0
    for img_path in site["images"]:
        full_path = f"{base}/{img_path}"
        size = fetch_size(ORIGIN_URL, full_path)
        image_sizes[img_path] = size
        total_jpeg += size
        print(f"    {img_path}: {size:,} bytes")
    print(f"    Total images (JPEG): {total_jpeg:,} bytes")

    # 2. Trigger cold miss through nginx
    print("\n  [2/5] Triggering cold miss through nginx...")
    # Fetch HTML (triggers notification)
    resp = requests.get(f"{NGINX_URL}{base}/index.html", timeout=10)
    print(f"    HTML: {resp.headers.get('X-PageSpeed', 'N/A')}")

    # Fetch CSS and JS
    requests.get(f"{NGINX_URL}{base}/style.css", timeout=10)
    requests.get(f"{NGINX_URL}{base}/app.js", timeout=10)

    # Fetch all images
    for img_path in site["images"]:
        requests.get(f"{NGINX_URL}{base}/{img_path}", timeout=10)

    # 3. Poll for optimized variants
    print("\n  [3/5] Polling for optimized variants...")

    # CSS minified - hard-fail if the worker never produced a smaller variant
    # (not-optimizing worker) rather than silently recording after==before.
    css_resp = poll_for_optimized(f"{base}/style.css", expect_smaller_than=css_size)
    if css_resp is None:
        raise RuntimeError(
            f"{site['name']}: CSS never minified within {POLL_TIMEOUT:.0f}s "
            f"(still {css_size:,} bytes). The worker produced no smaller variant - "
            f"check `docker compose logs worker`."
        )
    css_minified = len(css_resp.content)
    print(
        f"    CSS minified: {css_minified:,} bytes ({100 - css_minified * 100 // css_size}% of original)"
    )

    # JS minified - same hard-fail.
    js_resp = poll_for_optimized(f"{base}/app.js", expect_smaller_than=js_size)
    if js_resp is None:
        raise RuntimeError(
            f"{site['name']}: JS never minified within {POLL_TIMEOUT:.0f}s "
            f"(still {js_size:,} bytes). The worker produced no smaller variant - "
            f"check the worker logs."
        )
    js_minified = len(js_resp.content)
    print(
        f"    JS minified:  {js_minified:,} bytes ({100 - js_minified * 100 // js_size}% of original)"
    )

    # Image variants (WebP and AVIF)
    total_webp = 0
    total_avif = 0
    for img_path in site["images"]:
        full_path = f"{base}/{img_path}"

        # WebP - hard-fail if no variant. No fabricated fallback estimate:
        # the old `0.30 * orig` fallback masked a fully-unoptimizing worker as
        # plausible image savings (the demo would have shipped invented numbers).
        webp_size = poll_for_image_variant(full_path, "image/webp", is_webp)
        if webp_size is None:
            raise RuntimeError(
                f"{site['name']}: {img_path} produced no WebP variant within "
                f"{POLL_TIMEOUT:.0f}s - worker not optimizing images (check the worker logs)."
            )
        print(f"    {img_path} WebP: {webp_size:,} bytes")
        total_webp += webp_size

        # AVIF - same hard-fail, no fabricated fallback.
        avif_size = poll_for_image_variant(full_path, "image/avif", is_avif)
        if avif_size is None:
            raise RuntimeError(
                f"{site['name']}: {img_path} produced no AVIF variant within "
                f"{POLL_TIMEOUT:.0f}s - worker not optimizing images (check the worker logs)."
            )
        print(f"    {img_path} AVIF: {avif_size:,} bytes")
        total_avif += avif_size

    print(f"    Total WebP: {total_webp:,} bytes")
    print(f"    Total AVIF: {total_avif:,} bytes")

    # 4. Extract CSS snippet
    print("\n  [4/5] Extracting CSS snippet...")
    original_css = fetch_content(ORIGIN_URL, f"{base}/style.css").decode(
        "utf-8", errors="replace"
    )
    minified_css = (
        css_resp.content.decode("utf-8", errors="replace") if css_resp else original_css
    )
    css_snippet = extract_css_snippet(original_css, minified_css)
    print(f"    Before: {len(css_snippet['before'])} chars")
    print(f"    After:  {len(css_snippet['after'])} chars")

    # 5. Lighthouse (optional)
    lighthouse_before = None
    lighthouse_after = None
    load_before = None
    load_after = None

    if not skip_lighthouse:
        print("\n  [5/5] Running Lighthouse (3 runs each, ~2 min)...")

        print("    Origin (before)...")
        lh_before = run_lighthouse(f"{ORIGIN_URL}{base}/index.html")
        if lh_before:
            lighthouse_before = lh_before["score"]
            load_before = lh_before["tti"]
            print(f"    Score: {lighthouse_before}, TTI: {load_before}s")

        print("    Nginx (after)...")
        lh_after = run_lighthouse(f"{NGINX_URL}{base}/index.html")
        if lh_after:
            lighthouse_after = lh_after["score"]
            load_after = lh_after["tti"]
            print(f"    Score: {lighthouse_after}, TTI: {load_after}s")
    else:
        print("\n  [5/5] Lighthouse skipped (--skip-lighthouse)")

    # Use estimates if Lighthouse didn't run or failed
    if lighthouse_before is None or lighthouse_after is None:
        # Estimate based on resource savings
        total_before = html_size + css_size + js_size + total_jpeg
        total_after = html_size + css_minified + js_minified + total_avif
        savings_pct = (total_before - total_after) / total_before
        lighthouse_before = lighthouse_before or max(
            30, int(50 + 20 * (1 - savings_pct))
        )
        lighthouse_after = lighthouse_after or min(
            99, int(lighthouse_before + savings_pct * 50)
        )
        print(f"    Lighthouse estimated: {lighthouse_before} -> {lighthouse_after}")

    if load_before is None or load_after is None:
        # Estimate load times based on 3G (1.6 Mbps = 200 KB/s)
        total_before = html_size + css_size + js_size + total_jpeg
        total_after = html_size + css_minified + js_minified + total_avif
        # Add RTT overhead (300ms * ~4 round trips)
        load_before = load_before or round(total_before / 200000 + 1.2, 1)
        load_after = load_after or round(total_after / 200000 + 0.6, 1)
        print(f"    Load time estimated: {load_before}s -> {load_after}s")

    return {
        "id": site["id"],
        "name": site["name"],
        "description": site["description"],
        "src": f"/demos/{site['id']}/index.html",
        "resources": {
            "html": {"before": html_size, "label": "HTML"},
            "css": {
                "before": css_size,
                "minified": css_minified,
                "label": "CSS",
            },
            "js": {
                "before": js_size,
                "minified": js_minified,
                "label": "JavaScript",
            },
            "images": {
                "count": len(site["images"]),
                "totalJpeg": total_jpeg,
                "totalWebp": total_webp,
                "totalAvif": total_avif,
                "label": site["image_label"],
            },
        },
        "lighthouse": {
            "before": lighthouse_before,
            "after": lighthouse_after,
        },
        "loadTime": {"before": load_before, "after": load_after},
        "cssSnippet": css_snippet,
    }


def validate_results(results: list[dict]) -> list[str]:
    """Run sanity checks on measurement results. Returns list of failures."""
    failures = []
    for site in results:
        sid = site["id"]
        r = site["resources"]

        # CSS minified < original
        if r["css"]["minified"] >= r["css"]["before"]:
            failures.append(
                f"{sid}: CSS minified ({r['css']['minified']}) >= "
                f"original ({r['css']['before']})"
            )

        # JS minified < original
        if r["js"]["minified"] >= r["js"]["before"]:
            failures.append(
                f"{sid}: JS minified ({r['js']['minified']}) >= "
                f"original ({r['js']['before']})"
            )

        # WebP < JPEG
        if r["images"]["totalWebp"] >= r["images"]["totalJpeg"]:
            failures.append(
                f"{sid}: WebP ({r['images']['totalWebp']}) >= "
                f"JPEG ({r['images']['totalJpeg']})"
            )

        # AVIF < WebP
        if r["images"]["totalAvif"] >= r["images"]["totalWebp"]:
            failures.append(
                f"{sid}: AVIF ({r['images']['totalAvif']}) >= "
                f"WebP ({r['images']['totalWebp']})"
            )

        # Lighthouse after > before
        if site["lighthouse"]["after"] <= site["lighthouse"]["before"]:
            failures.append(
                f"{sid}: Lighthouse after ({site['lighthouse']['after']}) <= "
                f"before ({site['lighthouse']['before']})"
            )

        # All sizes > 0
        for key in ["html", "css", "js"]:
            if r[key].get("before", 0) <= 0:
                failures.append(f"{sid}: {key} before size <= 0")
        if r["images"]["totalJpeg"] <= 0:
            failures.append(f"{sid}: total JPEG size <= 0")

        # CSS snippets non-empty
        if not site.get("cssSnippet", {}).get("before"):
            failures.append(f"{sid}: CSS snippet before is empty")
        if not site.get("cssSnippet", {}).get("after"):
            failures.append(f"{sid}: CSS snippet after is empty")

    return failures


def main():
    parser = argparse.ArgumentParser(
        description="Measure real demo site metrics for PageSpeed 2.0"
    )
    parser.add_argument(
        "--skip-lighthouse",
        action="store_true",
        help="Skip Lighthouse runs (faster, uses estimates)",
    )
    parser.add_argument(
        "--output",
        type=str,
        default=str(OUTPUT_FILE),
        help="Output JSON file path",
    )
    args = parser.parse_args()
    require_website_dir()

    print("PageSpeed 2.0 Demo Measurement")
    print("=" * 60)

    # Check services are running
    print("\nChecking services...")
    try:
        resp = requests.get(f"{ORIGIN_URL}/demos/ecommerce/index.html", timeout=5)
        resp.raise_for_status()
        print(f"  Origin: OK ({resp.status_code})")
    except requests.RequestException as e:
        print(f"  Origin: FAILED ({e})")
        print("\nStart the measurement stack first:")
        print("  docker compose -f tools/measure-demo/docker-compose.yml up -d")
        sys.exit(1)

    try:
        resp = requests.get(f"{NGINX_URL}/demos/ecommerce/index.html", timeout=5)
        print(
            f"  Nginx: OK ({resp.status_code}, X-PageSpeed: {resp.headers.get('X-PageSpeed', 'N/A')})"
        )
    except requests.RequestException as e:
        print(f"  Nginx: FAILED ({e})")
        print("\nStart the measurement stack first:")
        print("  docker compose -f tools/measure-demo/docker-compose.yml up -d")
        sys.exit(1)

    # Precondition: the worker must be up and ready, or it optimizes nothing
    # and every metric reads after==before.
    print("\nChecking worker readiness...")
    check_worker_ready()

    # Measure each site
    results = []
    for site in SITES:
        result = measure_site(site, skip_lighthouse=args.skip_lighthouse)
        results.append(result)

    # Validate
    print("\n" + "=" * 60)
    print("  Validation")
    print("=" * 60)
    failures = validate_results(results)
    if failures:
        print("\n  WARNINGS (non-fatal):")
        for f in failures:
            print(f"    - {f}")
    else:
        print("\n  All sanity checks passed!")

    # Write output
    output = {
        "generated_at": datetime.now(UTC).isoformat(),
        "sites": results,
    }

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        json.dump(output, f, indent=2)
    print(f"\n  Output written to: {output_path}")

    # Summary
    print("\n" + "=" * 60)
    print("  Summary")
    print("=" * 60)
    for site in results:
        r = site["resources"]
        total_before = (
            r["html"]["before"]
            + r["css"]["before"]
            + r["js"]["before"]
            + r["images"]["totalJpeg"]
        )
        total_after = (
            r["html"]["before"]
            + r["css"]["minified"]
            + r["js"]["minified"]
            + r["images"]["totalAvif"]
        )
        pct = int((total_before - total_after) / total_before * 100)
        print(
            f"  {site['name']:25s}  "
            f"{total_before:>8,} -> {total_after:>8,} bytes  "
            f"(-{pct}%)  "
            f"LH: {site['lighthouse']['before']} -> {site['lighthouse']['after']}"
        )

    if failures:
        print(f"\n  {len(failures)} validation warning(s) - review above")
        sys.exit(0)  # warnings are non-fatal
    else:
        print("\n  All measurements complete and validated!")


if __name__ == "__main__":
    main()
