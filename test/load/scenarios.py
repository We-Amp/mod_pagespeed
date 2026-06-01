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

"""Load test scenario definitions for PageSpeed Envoy filter.

This module defines load test scenarios for benchmarking the Envoy
PageSpeed filter under various conditions.
"""

from dataclasses import dataclass, field
from enum import Enum
from typing import Callable, List, Optional


class LoadTool(Enum):
    """Available load testing tools."""
    WRK = "wrk"
    AB = "ab"  # Apache Bench


@dataclass
class LoadTestResult:
    """Results from a load test run.

    Attributes:
        requests_per_second: Throughput in requests per second
        latency_p50_ms: 50th percentile latency in milliseconds
        latency_p95_ms: 95th percentile latency in milliseconds
        latency_p99_ms: 99th percentile latency in milliseconds
        latency_avg_ms: Average latency in milliseconds
        latency_max_ms: Maximum latency in milliseconds
        error_rate: Percentage of failed requests (0.0 - 100.0)
        total_requests: Total number of requests completed
        total_errors: Total number of errors
        duration_seconds: Actual test duration
        bytes_per_second: Transfer rate in bytes per second
        memory_start_mb: Memory usage at test start (optional)
        memory_end_mb: Memory usage at test end (optional)
        memory_peak_mb: Peak memory usage during test (optional)
    """
    requests_per_second: float = 0.0
    latency_p50_ms: float = 0.0
    latency_p95_ms: float = 0.0
    latency_p99_ms: float = 0.0
    latency_avg_ms: float = 0.0
    latency_max_ms: float = 0.0
    error_rate: float = 0.0
    total_requests: int = 0
    total_errors: int = 0
    duration_seconds: float = 0.0
    bytes_per_second: float = 0.0
    memory_start_mb: Optional[float] = None
    memory_end_mb: Optional[float] = None
    memory_peak_mb: Optional[float] = None

    def summary(self) -> str:
        """Return a human-readable summary of the results."""
        lines = [
            f"Throughput: {self.requests_per_second:.2f} req/s",
            f"Latency p50: {self.latency_p50_ms:.2f} ms",
            f"Latency p95: {self.latency_p95_ms:.2f} ms",
            f"Latency p99: {self.latency_p99_ms:.2f} ms",
            f"Latency avg: {self.latency_avg_ms:.2f} ms",
            f"Latency max: {self.latency_max_ms:.2f} ms",
            f"Error rate: {self.error_rate:.2f}%",
            f"Total requests: {self.total_requests}",
            f"Duration: {self.duration_seconds:.2f}s",
        ]
        if self.memory_start_mb is not None:
            lines.append(f"Memory start: {self.memory_start_mb:.1f} MB")
        if self.memory_end_mb is not None:
            lines.append(f"Memory end: {self.memory_end_mb:.1f} MB")
        if self.memory_peak_mb is not None:
            lines.append(f"Memory peak: {self.memory_peak_mb:.1f} MB")
        return "\n".join(lines)


@dataclass
class ScenarioStep:
    """A single step in a load test scenario.

    Attributes:
        duration_seconds: How long to run this step
        connections: Number of concurrent connections
        threads: Number of threads (for wrk)
        rate_limit: Target requests per second (None = unlimited)
        url_suffix: URL suffix to append to base URL (e.g., query params)
        headers: Additional HTTP headers
        description: Human-readable description of this step
    """
    duration_seconds: int = 60
    connections: int = 100
    threads: int = 4
    rate_limit: Optional[int] = None
    url_suffix: str = ""
    headers: dict = field(default_factory=dict)
    description: str = ""


@dataclass
class LoadScenario:
    """A load test scenario with one or more steps.

    Attributes:
        name: Scenario identifier
        description: Human-readable description
        steps: List of scenario steps to execute
        url_path: URL path to test (relative to base URL)
        warmup_seconds: Warmup duration before collecting metrics
        cooldown_seconds: Cooldown duration after test
    """
    name: str
    description: str
    steps: List[ScenarioStep]
    url_path: str = "/"
    warmup_seconds: int = 5
    cooldown_seconds: int = 2


# Pre-defined scenarios

def baseline_scenario() -> LoadScenario:
    """Baseline scenario: 100 req/s sustained for 60 seconds.

    This scenario establishes a baseline performance measurement
    with moderate, sustained load.
    """
    return LoadScenario(
        name="baseline",
        description="Sustained moderate load (100 connections, 60 seconds)",
        url_path="/mod_pagespeed_example/",
        steps=[
            ScenarioStep(
                duration_seconds=60,
                connections=100,
                threads=4,
                description="Sustained 100 connections for 60s",
            ),
        ],
    )


def spike_scenario() -> LoadScenario:
    """Spike scenario: 100 to 1000 req/s ramp over 30 seconds.

    This scenario tests how the system handles a sudden increase
    in load, simulating traffic spikes.
    """
    steps = []
    # Ramp from 100 to 1000 connections over 30 seconds
    # Use 6 steps of 5 seconds each
    for i in range(6):
        connections = 100 + (i * 180)  # 100, 280, 460, 640, 820, 1000
        steps.append(
            ScenarioStep(
                duration_seconds=5,
                connections=connections,
                threads=min(connections // 25, 16),  # Scale threads with load
                description=f"Ramp step {i+1}/6: {connections} connections",
            )
        )
    # Hold at peak for 10 seconds
    steps.append(
        ScenarioStep(
            duration_seconds=10,
            connections=1000,
            threads=16,
            description="Hold at peak (1000 connections) for 10s",
        )
    )

    return LoadScenario(
        name="spike",
        description="Traffic spike: ramp from 100 to 1000 connections over 30s, hold 10s",
        url_path="/mod_pagespeed_example/",
        steps=steps,
        warmup_seconds=3,
    )


def html_heavy_scenario() -> LoadScenario:
    """HTML Heavy scenario: All responses trigger HTML rewriting.

    This scenario specifically tests HTML rewriting performance
    by requesting pages with PageSpeed filters enabled.
    """
    return LoadScenario(
        name="html_heavy",
        description="HTML rewriting intensive (PageSpeed filters enabled)",
        url_path="/mod_pagespeed_example/combine_css.html",
        steps=[
            ScenarioStep(
                duration_seconds=60,
                connections=50,
                threads=4,
                # Enable various HTML rewriting filters
                url_suffix="?PageSpeedFilters=+collapse_whitespace,+combine_css,+combine_javascript,+rewrite_css,+rewrite_javascript",
                description="HTML rewriting with multiple filters (50 connections)",
            ),
        ],
    )


def cache_hit_scenario() -> LoadScenario:
    """Cache Hit scenario: Repeated requests to same optimized resources.

    This scenario tests cache performance by repeatedly requesting
    the same optimized resources to maximize cache hit rate.
    """
    return LoadScenario(
        name="cache_hit",
        description="Cache hit intensive (repeated requests to optimized resources)",
        url_path="/mod_pagespeed_example/styles/yellow.css",
        steps=[
            # First, warm up the cache with a short burst
            ScenarioStep(
                duration_seconds=10,
                connections=10,
                threads=2,
                description="Cache warmup (10 connections, 10s)",
            ),
            # Then hit the cache hard
            ScenarioStep(
                duration_seconds=60,
                connections=200,
                threads=8,
                description="Cache hit load (200 connections, 60s)",
            ),
        ],
        warmup_seconds=0,  # The first step serves as warmup
    )


def ipro_scenario() -> LoadScenario:
    """IPRO scenario: In-Place Resource Optimization testing.

    This scenario tests the IPRO (In-Place Resource Optimization)
    pathway by requesting static resources that get optimized
    in-place without HTML rewriting.
    """
    return LoadScenario(
        name="ipro",
        description="IPRO testing (in-place resource optimization)",
        url_path="/mod_pagespeed_example/images/Puzzle.jpg",
        steps=[
            ScenarioStep(
                duration_seconds=60,
                connections=100,
                threads=4,
                # Request image with PageSpeed processing
                url_suffix="?PageSpeed=on",
                description="IPRO image optimization (100 connections)",
            ),
        ],
    )


def stress_scenario() -> LoadScenario:
    """Stress scenario: High load to find breaking point.

    This scenario gradually increases load to find the system's
    capacity limits and identify breaking points.
    """
    steps = []
    # Gradually increase load
    for connections in [100, 250, 500, 750, 1000, 1500, 2000]:
        steps.append(
            ScenarioStep(
                duration_seconds=30,
                connections=connections,
                threads=min(connections // 25, 32),
                description=f"Stress level: {connections} connections",
            )
        )

    return LoadScenario(
        name="stress",
        description="Stress test: gradually increase load to find capacity limits",
        url_path="/mod_pagespeed_example/",
        steps=steps,
        warmup_seconds=5,
        cooldown_seconds=5,
    )


def mixed_workload_scenario() -> LoadScenario:
    """Mixed workload scenario: Combination of different request types.

    This scenario simulates realistic traffic patterns with a mix
    of HTML pages, CSS, JavaScript, and images.
    """
    return LoadScenario(
        name="mixed",
        description="Mixed workload (HTML, CSS, JS, images)",
        url_path="/mod_pagespeed_example/",  # Will be varied per step
        steps=[
            # HTML pages with rewriting
            ScenarioStep(
                duration_seconds=30,
                connections=50,
                threads=4,
                url_suffix="combine_css.html?PageSpeedFilters=+combine_css",
                description="HTML with CSS combining (50 connections)",
            ),
            # Static CSS resources
            ScenarioStep(
                duration_seconds=30,
                connections=100,
                threads=4,
                url_suffix="styles/yellow.css",
                description="Static CSS (100 connections)",
            ),
            # Image resources
            ScenarioStep(
                duration_seconds=30,
                connections=100,
                threads=4,
                url_suffix="images/Puzzle.jpg",
                description="Image resources (100 connections)",
            ),
        ],
    )


# Registry of all available scenarios
SCENARIOS = {
    "baseline": baseline_scenario,
    "spike": spike_scenario,
    "html_heavy": html_heavy_scenario,
    "cache_hit": cache_hit_scenario,
    "ipro": ipro_scenario,
    "stress": stress_scenario,
    "mixed": mixed_workload_scenario,
}


def get_scenario(name: str) -> LoadScenario:
    """Get a scenario by name.

    Args:
        name: Scenario name

    Returns:
        LoadScenario instance

    Raises:
        KeyError: If scenario not found
    """
    if name not in SCENARIOS:
        available = ", ".join(sorted(SCENARIOS.keys()))
        raise KeyError(f"Unknown scenario '{name}'. Available: {available}")
    return SCENARIOS[name]()


def list_scenarios() -> List[str]:
    """Return list of available scenario names."""
    return sorted(SCENARIOS.keys())
