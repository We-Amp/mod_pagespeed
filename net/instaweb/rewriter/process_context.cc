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

#include "net/instaweb/rewriter/public/process_context.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "base/logging.h"
#include "google/protobuf/stubs/common.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/http/domain_registry.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/js/js_tokenizer.h"
using namespace google;  // NOLINT

namespace {

int construction_count = 0;

}

// Clean up valgrind-based memory-leak checks by deleting statically allocated
// data from various libraries.  This must be called both from unit-tests
// and from the Apache module, so that valgrind can be run on both of them.

namespace net_instaweb {

ProcessContext::ProcessContext()
    : js_tokenizer_patterns_(new pagespeed::js::JsTokenizerPatterns) {
  ++construction_count;
  // Note: Multiple ProcessContext instances may exist in test binaries that
  // link both test infrastructure (with RewriteTestBaseProcessContext) and
  // production code (with ApacheProcessContext). This is harmless in tests.
  // We use fprintf instead of CHECK/LOG because this may run during static
  // initialization before glog is initialized.
  if (construction_count > 1) {
    // Only warn in debug builds to avoid log spam in production.
#ifndef NDEBUG
    fprintf(stderr,
            "Warning: ProcessContext constructed %d times "
            "(expected once, but multiple is OK in tests).\n",
            construction_count);
#endif
  }

  domain_registry::Init();
  HtmlKeywords::Init();
}

ProcessContext::~ProcessContext() {
  // Clean up statics from third_party code first.

  // The protobuf shutdown infrastructure is lazily initialized in a threadsafe
  // manner.  See third_party/protobuf/src/google/protobuf/stubs/common.cc,
  // function InitShutdownFunctionsOnce.
  google::protobuf::ShutdownProtobufLibrary();

  HtmlKeywords::ShutDown();
}

}  // namespace net_instaweb
