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

"""Envoy-specific PageSpeed filter integration tests.

This package contains tests that are specific to the Envoy PageSpeed filter.
These tests verify Envoy-specific behavior such as:
- Filter loading and initialization
- HTML rewriting with PageSpeedFilters parameter
- IPRO (In-Place Resource Optimization) via Envoy
- Envoy-specific headers and request flow
"""
