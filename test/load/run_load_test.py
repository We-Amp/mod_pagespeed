#!/usr/bin/env python3
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Load testing framework for PageSpeed Envoy filter.

This script runs load tests against a PageSpeed-enabled server using
wrk or Apache Bench (ab) and collects performance metrics.

Usage:
    # Run baseline scenario against local Envoy
    ./run_load_test.py --url http://localhost:8080 --scenario baseline

    # Run spike test with Apache Bench
    ./run_load_test.py --url http://localhost:8080 --scenario spike --tool ab

    # Run all scenarios and output JSON
    ./run_load_test.py --url http://localhost:8080 --all --output results.json

    # Track memory usage of envoy process
    ./run_load_test.py --url http://localhost:8080 --scenario baseline --pid 12345

Environment Variables:
    PAGESPEED_HOST: Server hostname (default: localhost)
    PAGESPEED_PORT: Server port (default: 8080)
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import asdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent))

from scenarios import (
    LoadScenario,
    LoadTestResult,
    LoadTool,
    ScenarioStep,
    get_scenario,
    list_scenarios,
)


class MemoryMonitor:
    """Monitor memory usage of a process during load testing."""

    def __init__(self, pid: int, interval: float = 0.5):
        """Initialize memory monitor.

        Args:
            pid: Process ID to monitor
            interval: Sampling interval in seconds
        """
        self.pid = pid
        self.interval = interval
        self.samples: List[float] = []
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def _get_memory_mb(self) -> Optional[float]:
        """Get current memory usage in MB."""
        try:
            # Try /proc on Linux
            proc_path = f"/proc/{self.pid}/status"
            if os.path.exists(proc_path):
                with open(proc_path, "r") as f:
                    for line in f:
                        if line.startswith("VmRSS:"):
                            # Parse "VmRSS:   12345 kB"
                            parts = line.split()
                            if len(parts) >= 2:
                                return float(parts[1]) / 1024  # kB to MB
            # Fallback to ps command
            result = subprocess.run(
                ["ps", "-o", "rss=", "-p", str(self.pid)],
                capture_output=True,
                text=True,
            )
            if result.returncode == 0 and result.stdout.strip():
                return float(result.stdout.strip()) / 1024  # kB to MB
        except (OSError, ValueError, FileNotFoundError):
            pass
        return None

    def _monitor_loop(self):
        """Background thread to collect memory samples."""
        while not self._stop.is_set():
            mem = self._get_memory_mb()
            if mem is not None:
                self.samples.append(mem)
            self._stop.wait(self.interval)

    def start(self):
        """Start monitoring in background thread."""
        self.samples = []
        self._stop.clear()
        self._thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self._thread.start()

    def stop(self) -> Tuple[Optional[float], Optional[float], Optional[float]]:
        """Stop monitoring and return (start, end, peak) memory in MB."""
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)
        if not self.samples:
            return None, None, None
        return self.samples[0], self.samples[-1], max(self.samples)


def check_tool_available(tool: LoadTool) -> bool:
    """Check if load testing tool is available."""
    cmd = "wrk" if tool == LoadTool.WRK else "ab"
    return shutil.which(cmd) is not None


def parse_wrk_output(output: str) -> LoadTestResult:
    """Parse wrk output into LoadTestResult.

    Example wrk output:
      Running 30s test @ http://localhost:8080/
        4 threads and 100 connections
        Thread Stats   Avg      Stdev     Max   +/- Stdev
          Latency     5.23ms    2.12ms  45.67ms   74.23%
          Req/Sec     4.82k   312.45     5.89k    68.75%
        Latency Distribution
           50%    4.89ms
           75%    6.12ms
           90%    7.89ms
           99%   12.34ms
        576432 requests in 30.00s, 123.45MB read
        Non-2xx or 3xx responses: 123
      Requests/sec:  19214.40
      Transfer/sec:      4.11MB
    """
    result = LoadTestResult()

    # Parse requests/sec
    match = re.search(r"Requests/sec:\s+([\d.]+)", output)
    if match:
        result.requests_per_second = float(match.group(1))

    # Parse latency stats (Avg, Max from Thread Stats)
    match = re.search(r"Latency\s+([\d.]+)(\w+)\s+([\d.]+)(\w+)\s+([\d.]+)(\w+)", output)
    if match:
        avg_val, avg_unit = float(match.group(1)), match.group(2)
        max_val, max_unit = float(match.group(5)), match.group(6)
        result.latency_avg_ms = _convert_time_to_ms(avg_val, avg_unit)
        result.latency_max_ms = _convert_time_to_ms(max_val, max_unit)

    # Parse latency distribution
    match = re.search(r"50%\s+([\d.]+)(\w+)", output)
    if match:
        result.latency_p50_ms = _convert_time_to_ms(float(match.group(1)), match.group(2))

    match = re.search(r"90%\s+([\d.]+)(\w+)", output)
    if match:
        # Use 90th percentile as proxy for 95th if 95th not available
        result.latency_p95_ms = _convert_time_to_ms(float(match.group(1)), match.group(2))

    match = re.search(r"99%\s+([\d.]+)(\w+)", output)
    if match:
        result.latency_p99_ms = _convert_time_to_ms(float(match.group(1)), match.group(2))

    # Parse total requests
    match = re.search(r"(\d+)\s+requests in", output)
    if match:
        result.total_requests = int(match.group(1))

    # Parse non-2xx/3xx responses (errors)
    match = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", output)
    if match:
        result.total_errors = int(match.group(1))
        if result.total_requests > 0:
            result.error_rate = (result.total_errors / result.total_requests) * 100

    # Also check for socket errors
    match = re.search(r"Socket errors:\s+connect\s+(\d+),\s+read\s+(\d+),\s+write\s+(\d+),\s+timeout\s+(\d+)", output)
    if match:
        socket_errors = sum(int(x) for x in match.groups())
        result.total_errors += socket_errors
        if result.total_requests > 0:
            result.error_rate = (result.total_errors / result.total_requests) * 100

    # Parse duration
    match = re.search(r"(\d+(?:\.\d+)?)[sm]?\s+test", output)
    if match:
        result.duration_seconds = float(match.group(1))

    # Parse transfer rate
    match = re.search(r"Transfer/sec:\s+([\d.]+)(\w+)", output)
    if match:
        val, unit = float(match.group(1)), match.group(2)
        result.bytes_per_second = _convert_bytes(val, unit)

    return result


def parse_ab_output(output: str) -> LoadTestResult:
    """Parse Apache Bench output into LoadTestResult.

    Example ab output:
      Server Software:        Apache
      Server Hostname:        localhost
      Server Port:            8080

      Document Path:          /
      Document Length:        12345 bytes

      Concurrency Level:      100
      Time taken for tests:   30.123 seconds
      Complete requests:      50000
      Failed requests:        0
      Total transferred:      617500000 bytes
      HTML transferred:       617500000 bytes
      Requests per second:    1659.90 [#/sec] (mean)
      Time per request:       60.245 [ms] (mean)
      Time per request:       0.602 [ms] (mean, across all concurrent requests)
      Transfer rate:          20012.34 [Kbytes/sec] received

      Connection Times (ms)
                    min  mean[+/-sd] median   max
      Connect:        0    1   0.5      1       5
      Processing:    10   59  15.2     57     200
      Waiting:        5   55  14.8     53     195
      Total:         10   60  15.3     58     202

      Percentage of the requests served within a certain time (ms)
        50%     58
        66%     65
        75%     70
        80%     74
        90%     82
        95%     90
        98%    105
        99%    120
       100%    202 (longest request)
    """
    result = LoadTestResult()

    # Parse requests/sec
    match = re.search(r"Requests per second:\s+([\d.]+)", output)
    if match:
        result.requests_per_second = float(match.group(1))

    # Parse mean time per request
    match = re.search(r"Time per request:\s+([\d.]+)\s+\[ms\]\s+\(mean\)", output)
    if match:
        result.latency_avg_ms = float(match.group(1))

    # Parse complete/failed requests
    match = re.search(r"Complete requests:\s+(\d+)", output)
    if match:
        result.total_requests = int(match.group(1))

    match = re.search(r"Failed requests:\s+(\d+)", output)
    if match:
        result.total_errors = int(match.group(1))
        if result.total_requests > 0:
            result.error_rate = (result.total_errors / result.total_requests) * 100

    # Parse duration
    match = re.search(r"Time taken for tests:\s+([\d.]+)\s+seconds", output)
    if match:
        result.duration_seconds = float(match.group(1))

    # Parse transfer rate
    match = re.search(r"Transfer rate:\s+([\d.]+)\s+\[Kbytes/sec\]", output)
    if match:
        result.bytes_per_second = float(match.group(1)) * 1024

    # Parse percentile latencies from the table
    match = re.search(r"50%\s+(\d+)", output)
    if match:
        result.latency_p50_ms = float(match.group(1))

    match = re.search(r"95%\s+(\d+)", output)
    if match:
        result.latency_p95_ms = float(match.group(1))

    match = re.search(r"99%\s+(\d+)", output)
    if match:
        result.latency_p99_ms = float(match.group(1))

    match = re.search(r"100%\s+(\d+)", output)
    if match:
        result.latency_max_ms = float(match.group(1))

    return result


def _convert_time_to_ms(value: float, unit: str) -> float:
    """Convert time value to milliseconds."""
    unit = unit.lower()
    if unit in ("us", "microsecond", "microseconds"):
        return value / 1000
    elif unit in ("ms", "millisecond", "milliseconds"):
        return value
    elif unit in ("s", "second", "seconds"):
        return value * 1000
    elif unit in ("m", "minute", "minutes"):
        return value * 60 * 1000
    return value  # Assume ms


def _convert_bytes(value: float, unit: str) -> float:
    """Convert byte value to bytes."""
    unit = unit.upper()
    if unit in ("B", "BYTES"):
        return value
    elif unit in ("KB", "K"):
        return value * 1024
    elif unit in ("MB", "M"):
        return value * 1024 * 1024
    elif unit in ("GB", "G"):
        return value * 1024 * 1024 * 1024
    return value


def run_wrk_test(
    url: str,
    duration: int,
    connections: int,
    threads: int,
    headers: Optional[Dict[str, str]] = None,
    rate_limit: Optional[int] = None,
    lua_script: Optional[str] = None,
) -> Tuple[LoadTestResult, str]:
    """Run wrk load test and return results.

    Args:
        url: Full URL to test
        duration: Test duration in seconds
        connections: Number of concurrent connections
        threads: Number of threads
        headers: Additional HTTP headers
        rate_limit: Target requests per second (requires wrk2)
        lua_script: Path to Lua script for custom logic

    Returns:
        Tuple of (LoadTestResult, raw output string)
    """
    cmd = ["wrk"]

    # Basic options
    cmd.extend(["-t", str(threads)])
    cmd.extend(["-c", str(connections)])
    cmd.extend(["-d", f"{duration}s"])

    # Add latency distribution output
    cmd.append("--latency")

    # Add headers
    if headers:
        for name, value in headers.items():
            cmd.extend(["-H", f"{name}: {value}"])

    # Rate limiting (wrk2 only)
    if rate_limit:
        cmd.extend(["-R", str(rate_limit)])

    # Lua script
    if lua_script:
        cmd.extend(["-s", lua_script])

    # URL must be last
    cmd.append(url)

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=duration + 30,  # Extra time for startup/shutdown
        )
        output = result.stdout + result.stderr
        return parse_wrk_output(output), output
    except subprocess.TimeoutExpired:
        return LoadTestResult(), "ERROR: wrk timed out"
    except FileNotFoundError:
        return LoadTestResult(), "ERROR: wrk not found. Install with: apt-get install wrk"


def run_ab_test(
    url: str,
    duration: int,
    connections: int,
    headers: Optional[Dict[str, str]] = None,
) -> Tuple[LoadTestResult, str]:
    """Run Apache Bench load test and return results.

    Args:
        url: Full URL to test
        duration: Test duration in seconds
        connections: Number of concurrent connections (concurrency level)
        headers: Additional HTTP headers

    Returns:
        Tuple of (LoadTestResult, raw output string)
    """
    cmd = ["ab"]

    # Use timelimit instead of number of requests
    cmd.extend(["-t", str(duration)])

    # Concurrency level
    cmd.extend(["-c", str(connections)])

    # Don't stop on errors
    cmd.append("-r")

    # Add headers
    if headers:
        for name, value in headers.items():
            cmd.extend(["-H", f"{name}: {value}"])

    # URL must be last
    cmd.append(url)

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=duration + 30,
        )
        output = result.stdout + result.stderr
        return parse_ab_output(output), output
    except subprocess.TimeoutExpired:
        return LoadTestResult(), "ERROR: ab timed out"
    except FileNotFoundError:
        return LoadTestResult(), "ERROR: ab not found. Install with: apt-get install apache2-utils"


def run_scenario_step(
    base_url: str,
    step: ScenarioStep,
    url_path: str,
    tool: LoadTool,
    verbose: bool = False,
) -> Tuple[LoadTestResult, str]:
    """Run a single scenario step.

    Args:
        base_url: Base URL (e.g., http://localhost:8080)
        step: Scenario step to execute
        url_path: URL path from scenario
        tool: Load testing tool to use
        verbose: Print verbose output

    Returns:
        Tuple of (LoadTestResult, raw output string)
    """
    # Build full URL
    full_path = url_path + step.url_suffix
    url = base_url.rstrip("/") + "/" + full_path.lstrip("/")

    if verbose:
        print(f"  Running: {step.description}")
        print(f"  URL: {url}")
        print(f"  Connections: {step.connections}, Duration: {step.duration_seconds}s")

    if tool == LoadTool.WRK:
        return run_wrk_test(
            url=url,
            duration=step.duration_seconds,
            connections=step.connections,
            threads=step.threads,
            headers=step.headers,
            rate_limit=step.rate_limit,
        )
    else:  # LoadTool.AB
        return run_ab_test(
            url=url,
            duration=step.duration_seconds,
            connections=step.connections,
            headers=step.headers,
        )


def run_scenario(
    scenario: LoadScenario,
    base_url: str,
    tool: LoadTool,
    monitor_pid: Optional[int] = None,
    verbose: bool = False,
) -> List[LoadTestResult]:
    """Run a complete load test scenario.

    Args:
        scenario: Scenario to execute
        base_url: Base URL of target server
        tool: Load testing tool to use
        monitor_pid: Optional PID to monitor for memory usage
        verbose: Print verbose output

    Returns:
        List of LoadTestResult for each step
    """
    results = []

    # Setup memory monitoring if requested
    mem_monitor = None
    if monitor_pid:
        mem_monitor = MemoryMonitor(monitor_pid)
        mem_monitor.start()

    print(f"\nRunning scenario: {scenario.name}")
    print(f"Description: {scenario.description}")
    print(f"Steps: {len(scenario.steps)}")
    print("-" * 60)

    # Warmup period
    if scenario.warmup_seconds > 0:
        print(f"Warming up for {scenario.warmup_seconds}s...")
        time.sleep(scenario.warmup_seconds)

    # Run each step
    for i, step in enumerate(scenario.steps, 1):
        print(f"\nStep {i}/{len(scenario.steps)}: {step.description}")

        result, output = run_scenario_step(
            base_url=base_url,
            step=step,
            url_path=scenario.url_path,
            tool=tool,
            verbose=verbose,
        )

        if verbose:
            print(output)

        # Add memory stats if monitoring
        if mem_monitor:
            mem_start, mem_end, mem_peak = mem_monitor.stop()
            result.memory_start_mb = mem_start
            result.memory_end_mb = mem_end
            result.memory_peak_mb = mem_peak
            # Restart monitoring for next step
            mem_monitor.start()

        results.append(result)

        # Print summary
        print(f"  Throughput: {result.requests_per_second:.2f} req/s")
        print(f"  Latency p50/p95/p99: {result.latency_p50_ms:.1f}/{result.latency_p95_ms:.1f}/{result.latency_p99_ms:.1f} ms")
        print(f"  Errors: {result.error_rate:.2f}%")

    # Cooldown period
    if scenario.cooldown_seconds > 0:
        print(f"\nCooling down for {scenario.cooldown_seconds}s...")
        time.sleep(scenario.cooldown_seconds)

    # Stop memory monitoring
    if mem_monitor:
        mem_monitor.stop()

    return results


def aggregate_results(results: List[LoadTestResult]) -> LoadTestResult:
    """Aggregate multiple step results into a summary.

    Args:
        results: List of step results

    Returns:
        Aggregated LoadTestResult
    """
    if not results:
        return LoadTestResult()

    total_requests = sum(r.total_requests for r in results)
    total_errors = sum(r.total_errors for r in results)
    total_duration = sum(r.duration_seconds for r in results)

    # Weight latencies by request count
    def weighted_avg(attr: str) -> float:
        total_weight = sum(r.total_requests for r in results if getattr(r, attr) > 0)
        if total_weight == 0:
            return 0.0
        return sum(getattr(r, attr) * r.total_requests for r in results) / total_weight

    return LoadTestResult(
        requests_per_second=total_requests / total_duration if total_duration > 0 else 0,
        latency_p50_ms=weighted_avg("latency_p50_ms"),
        latency_p95_ms=weighted_avg("latency_p95_ms"),
        latency_p99_ms=weighted_avg("latency_p99_ms"),
        latency_avg_ms=weighted_avg("latency_avg_ms"),
        latency_max_ms=max((r.latency_max_ms for r in results), default=0),
        error_rate=(total_errors / total_requests * 100) if total_requests > 0 else 0,
        total_requests=total_requests,
        total_errors=total_errors,
        duration_seconds=total_duration,
        bytes_per_second=sum(r.bytes_per_second * r.duration_seconds for r in results) / total_duration if total_duration > 0 else 0,
        memory_start_mb=results[0].memory_start_mb if results else None,
        memory_end_mb=results[-1].memory_end_mb if results else None,
        memory_peak_mb=max((r.memory_peak_mb for r in results if r.memory_peak_mb), default=None),
    )


def main():
    """Main entry point for load testing."""
    parser = argparse.ArgumentParser(
        description="Load testing framework for PageSpeed Envoy filter",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run baseline scenario
  %(prog)s --url http://localhost:8080 --scenario baseline

  # Run all scenarios
  %(prog)s --url http://localhost:8080 --all

  # Run with memory monitoring
  %(prog)s --url http://localhost:8080 --scenario stress --pid 12345

  # Output results as JSON
  %(prog)s --url http://localhost:8080 --scenario baseline --output results.json

Available scenarios:
""" + "\n".join(f"  {name}" for name in list_scenarios()),
    )

    parser.add_argument(
        "--url",
        default=None,
        help="Base URL of target server (default: from env or http://localhost:8080)",
    )
    parser.add_argument(
        "--scenario",
        choices=list_scenarios(),
        help="Scenario to run",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Run all scenarios",
    )
    parser.add_argument(
        "--tool",
        choices=["wrk", "ab"],
        default="wrk",
        help="Load testing tool to use (default: wrk)",
    )
    parser.add_argument(
        "--pid",
        type=int,
        help="PID of process to monitor for memory usage",
    )
    parser.add_argument(
        "--output",
        "-o",
        help="Output file for JSON results",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Print verbose output",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available scenarios and exit",
    )

    args = parser.parse_args()

    # List scenarios
    if args.list:
        print("Available scenarios:")
        for name in list_scenarios():
            scenario = get_scenario(name)
            print(f"  {name}: {scenario.description}")
        return 0

    # Validate arguments
    if not args.scenario and not args.all:
        parser.error("Either --scenario or --all is required")

    # Determine base URL
    if args.url:
        base_url = args.url
    else:
        host = os.environ.get("PAGESPEED_HOST", "localhost")
        port = os.environ.get("PAGESPEED_PORT", "8080")
        base_url = f"http://{host}:{port}"

    # Determine tool
    tool = LoadTool.WRK if args.tool == "wrk" else LoadTool.AB

    # Check tool availability
    if not check_tool_available(tool):
        tool_cmd = "wrk" if tool == LoadTool.WRK else "ab"
        print(f"ERROR: {tool_cmd} not found in PATH", file=sys.stderr)
        if tool == LoadTool.WRK:
            print("Install with: apt-get install wrk", file=sys.stderr)
        else:
            print("Install with: apt-get install apache2-utils", file=sys.stderr)
        return 1

    print(f"Target URL: {base_url}")
    print(f"Load tool: {tool.value}")
    if args.pid:
        print(f"Monitoring PID: {args.pid}")

    # Collect all results
    all_results: Dict[str, Dict] = {}

    # Run scenarios
    scenarios_to_run = list_scenarios() if args.all else [args.scenario]

    for scenario_name in scenarios_to_run:
        scenario = get_scenario(scenario_name)
        step_results = run_scenario(
            scenario=scenario,
            base_url=base_url,
            tool=tool,
            monitor_pid=args.pid,
            verbose=args.verbose,
        )

        # Aggregate results
        aggregate = aggregate_results(step_results)

        all_results[scenario_name] = {
            "scenario": {
                "name": scenario.name,
                "description": scenario.description,
                "url_path": scenario.url_path,
                "steps": len(scenario.steps),
            },
            "aggregate": asdict(aggregate),
            "steps": [asdict(r) for r in step_results],
        }

        # Print summary
        print("\n" + "=" * 60)
        print(f"Scenario '{scenario_name}' Summary:")
        print(aggregate.summary())

    # Output JSON if requested
    if args.output:
        output_data = {
            "base_url": base_url,
            "tool": tool.value,
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "results": all_results,
        }
        with open(args.output, "w") as f:
            json.dump(output_data, f, indent=2)
        print(f"\nResults written to: {args.output}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
