#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Zero-Copy Benchmark: PageSpeed Cache vs Static Nginx
#
# Measures steady-state cached serving throughput using fortio.
# Compares PageSpeed's mmap-based zero-copy cache serving against
# nginx's native sendfile() for the same static content.
#
# NOTE: Requires Linux Docker (--network host). Does not work on
# macOS/Windows Docker Desktop where --network host is unsupported.
#
# Usage:
#   ./tools/benchmark/run_benchmark.sh
#   ./tools/benchmark/run_benchmark.sh --duration 30s --connections 32

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Defaults (overridable via env vars or flags)
DURATION="${DURATION:-10s}"
CONNECTIONS="${CONNECTIONS:-16}"
BASELINE_PORT=8086
PAGESPEED_PORT=8085
WARMUP_TIMEOUT=120  # seconds
WARMUP_POLL_INTERVAL=2  # seconds

# Parse flags
while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)
            [[ $# -ge 2 ]] || { echo "ERROR: --duration requires a value"; exit 1; }
            DURATION="$2"; shift 2 ;;
        --connections)
            [[ $# -ge 2 ]] || { echo "ERROR: --connections requires a value"; exit 1; }
            CONNECTIONS="$2"; shift 2 ;;
        *) echo "Unknown flag: $1"; exit 1 ;;
    esac
done

# Test files: name:display_size — single source of truth.
# The Python report script reads a manifest file generated from this list.
FILES=(
    "1k.html:1KB"
    "10k.html:10KB"
    "100k.html:100KB"
    "10k.jpg:10KB"
    "100k.jpg:100KB"
    "1m.jpg:1MB"
)

# --- Helpers ---

log() { echo "==> $*"; }

cleanup() {
    log "Stopping containers..."
    docker compose down -v --remove-orphans 2>/dev/null || true
}

wait_for_http() {
    local url="$1"
    local label="$2"
    local deadline=$((SECONDS + WARMUP_TIMEOUT))
    while ! curl -sf -o /dev/null "$url" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            echo "ERROR: $label did not become ready within ${WARMUP_TIMEOUT}s"
            exit 1
        fi
        sleep 1
    done
    log "$label is ready"
}

warm_cache() {
    # Request each file through PageSpeed nginx and poll until X-PageSpeed: HIT
    local file="$1"
    local url="http://localhost:${PAGESPEED_PORT}/${file}"
    local deadline=$((SECONDS + WARMUP_TIMEOUT))

    # Initial request (triggers MISS → origin fetch → worker notification)
    curl -sf -o /dev/null "$url" 2>/dev/null || true
    sleep 1

    # Poll for HIT
    while true; do
        local header
        header=$(curl -sf -D - -o /dev/null "$url" 2>/dev/null | grep -i "X-PageSpeed:" || true)
        if echo "$header" | grep -qi "HIT"; then
            return 0
        fi
        if (( SECONDS >= deadline )); then
            echo "ERROR: $file did not reach HIT state within ${WARMUP_TIMEOUT}s"
            echo "       Benchmark results would be unreliable (MISS responses)."
            return 1
        fi
        sleep "$WARMUP_POLL_INTERVAL"
    done
}

run_fortio() {
    local url="$1"
    local output_file="$2"
    # --network host: fortio container shares host network to reach localhost ports.
    # Only works on Linux Docker; macOS/Windows Docker Desktop not supported.
    # -H "Accept-Encoding:" clears fortio's default "gzip" Accept-Encoding so
    # PageSpeed sees Identity encoding, matching the stored alternate mask (0x08).
    # Without this, every request is a non-exact-match triggering a blocking
    # notification send to the worker (~3ms per request).
    docker run --rm --network host fortio/fortio load \
        -qps 0 \
        -c "$CONNECTIONS" \
        -t "$DURATION" \
        -H "Accept-Encoding:" \
        -json - \
        "$url" > "$output_file" 2>"${output_file}.log"
}

# --- Main ---

RESULTS_DIR=$(mktemp -d)
trap 'cleanup; rm -rf "$RESULTS_DIR"' EXIT

# Step 1: Generate test data if missing
if [ ! -d "$SCRIPT_DIR/benchdata" ] || [ ! -f "$SCRIPT_DIR/benchdata/1k.html" ]; then
    log "Generating benchmark test data..."
    python3 "$SCRIPT_DIR/generate_bench_data.py"
fi

# Step 2: Start containers
log "Starting benchmark containers..."
docker compose down --remove-orphans 2>/dev/null || true
docker compose up -d --build

# Step 3: Wait for both nginx instances
log "Waiting for services..."
wait_for_http "http://localhost:${BASELINE_PORT}/1k.html" "nginx-baseline"
wait_for_http "http://localhost:${PAGESPEED_PORT}/1k.html" "nginx-pagespeed"

# Step 4: Pre-pull fortio image
log "Pulling fortio image..."
docker pull fortio/fortio > /dev/null 2>&1 || { echo "ERROR: Could not pull fortio/fortio image"; exit 1; }

# Step 5: Warm the cache
log "Warming PageSpeed cache..."
for entry in "${FILES[@]}"; do
    file="${entry%%:*}"
    warm_cache "$file"
    log "  $file cached"
done
log "Cache warm-up complete"

# Step 6: Run benchmarks and save JSON results to temp files
log "Running fortio benchmarks (duration=${DURATION}, connections=${CONNECTIONS})..."
echo ""

# Write manifest for the Python report script (single source of truth)
MANIFEST="${RESULTS_DIR}/manifest.txt"
for entry in "${FILES[@]}"; do
    echo "$entry" >> "$MANIFEST"
done

for entry in "${FILES[@]}"; do
    file="${entry%%:*}"
    safe="${file//\//_}"
    log "  Benchmarking $file..."

    run_fortio "http://localhost:${BASELINE_PORT}/${file}" "${RESULTS_DIR}/baseline_${safe}.json"
    run_fortio "http://localhost:${PAGESPEED_PORT}/${file}" "${RESULTS_DIR}/pagespeed_${safe}.json"
done

# Step 7: Print results table using a single Python invocation
python3 - "$RESULTS_DIR" <<'REPORT_SCRIPT'
import json, sys, os

results_dir = sys.argv[1]

# Read file list from manifest (shared with bash)
files = []
manifest_path = os.path.join(results_dir, "manifest.txt")
with open(manifest_path) as f:
    for line in f:
        line = line.strip()
        if line:
            name, size = line.split(":")
            files.append((name, size))

def load(path):
    try:
        with open(path) as f:
            data = json.load(f)
        if not data.get("ActualQPS"):
            print(f"WARNING: No QPS data in {os.path.basename(path)}", file=sys.stderr)
        return data
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"WARNING: Could not load {os.path.basename(path)}: {e}", file=sys.stderr)
        return {}

def get_percentile(data, pct):
    percs = data.get("DurationHistogram", {}).get("Percentiles", [])
    for p in percs:
        if p["Percentile"] == pct:
            return p["Value"]
    return 0

print()
print("=== PageSpeed Zero-Copy Benchmark ===")
print()
header = f"{'File':<14} {'Size':>6}  {'Baseline QPS':>14}  {'PageSpeed QPS':>14}  {'Ratio':>7}  {'p50 Base':>10} {'p50 PS':>10}  {'p99 Base':>10} {'p99 PS':>10}"
print(header)
print("-" * len(header))

for fname, size in files:
    safe = fname.replace("/", "_")
    b = load(os.path.join(results_dir, f"baseline_{safe}.json"))
    p = load(os.path.join(results_dir, f"pagespeed_{safe}.json"))

    b_qps = b.get("ActualQPS", 0)
    p_qps = p.get("ActualQPS", 0)

    if b_qps > 0:
        ratio = p_qps / b_qps
        ratio_str = f"{ratio:>6.2f}x"
    else:
        ratio_str = "   N/A "

    b_p50 = get_percentile(b, 50) * 1000
    p_p50 = get_percentile(p, 50) * 1000
    b_p99 = get_percentile(b, 99) * 1000
    p_p99 = get_percentile(p, 99) * 1000

    print(f"{fname:<14} {size:>6}  {b_qps:>14,.0f}  {p_qps:>14,.0f}  {ratio_str}  {b_p50:>9.2f}ms {p_p50:>9.2f}ms  {b_p99:>9.2f}ms {p_p99:>9.2f}ms")

print()
REPORT_SCRIPT

log "Benchmark complete."
