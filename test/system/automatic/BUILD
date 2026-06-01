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

licenses(["notice"])  # Apache 2

load("//bazel:pagespeed_python_test.bzl", "pagespeed_py_test", "pagespeed_py_test_suite")

# =============================================================================
# Python System Tests - Automatic
# =============================================================================
#
# These tests are ported from pagespeed/automatic/system_tests/*.sh
#
# Run all tests:
#   bazel test //test/system/automatic:all --test_output=streamed \
#       --test_env=PAGESPEED_HOST=localhost --test_env=PAGESPEED_PORT=8080
#
# Run a specific test:
#   bazel test //test/system/automatic:test_sanity --test_output=streamed \
#       --test_env=PAGESPEED_HOST=localhost --test_env=PAGESPEED_PORT=8080

# Test suite containing all automatic tests
pagespeed_py_test_suite(
    name = "all",
    srcs = [
        "test_add_instrumentation.py",
        "test_broken_images.py",
        "test_char_tweaks.py",
        "test_combiners.py",
        "test_content_length.py",
        "test_convert_meta_tags.py",
        "test_css_images.py",
        "test_css_sprite.py",
        "test_dedup_inlined_images.py",
        "test_defer_javascript.py",
        "test_elide_attributes.py",
        "test_extend_cache.py",
        "test_filters.py",
        "test_flatten_css_imports.py",
        "test_headers.py",
        "test_https.py",
        "test_image_resize.py",
        "test_inliners.py",
        "test_inline_preview_images.py",
        "test_insert_dns_prefetch.py",
        "test_lazyload.py",
        "test_local_storage_cache.py",
        "test_move_css.py",
        "test_no_cache.py",
        "test_outliners.py",
        "test_rel_canonical.py",
        "test_responsive_images.py",
        "test_rewrite_css.py",
        "test_rewrite_images.py",
        "test_rewrite_javascript.py",
        "test_sanity.py",
        "test_shortcut_icons.py",
        "test_webp.py",
    ],
    deps = ["//test/system:conftest"],
)
