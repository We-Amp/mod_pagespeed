// SPDX-License-Identifier: Apache-2.0
// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-html-kernel.sh.
// Canonical source: mod_pagespeed 1.15, pagespeed/kernel/html/html_filter.cc (#1130).
// Synced from the commit pinned in lib/html/HTML_KERNEL_PIN.
// Drift guard: CI re-runs tools/sync-html-kernel.sh and byte-compares.
// The canonical file's original license header is retained in full below;
// Apache-2.0 headed files: see lib/html/LICENSE.apache-2.0 for the license text.

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "lib/html/html_filter.h"

namespace net_instaweb {

HtmlFilter::HtmlFilter() : is_enabled_(true) {}

HtmlFilter::~HtmlFilter() {}

void HtmlFilter::RenderDone() {}

}  // namespace net_instaweb
