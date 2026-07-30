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

//
//
// CPU: Intel Nehalem with HyperThreading (4 cores) dL1:32KB dL2:256KB
// Benchmark                     Time(ns)    CPU(ns) Iterations
// ------------------------------------------------------------
// BM_MinifyJavascript/64            3862       3870     178281
// BM_MinifyJavascript/512          29962      30058      24922
// BM_MinifyJavascript/4k          163436     163944       4218
// BM_MinifyJavascript/32k        1370666    1374490        494
// BM_MinifyJavascript/256k      11499929   11532620        100
//
// Disclaimer: comparing runs over time and across different machines
// can be misleading.  When contemplating an algorithm change, always do
// interleaved runs with the old & new algorithm.

#include "benchmark/benchmark.h"
#include "net/instaweb/rewriter/public/javascript_code_block.h"
#include "net/instaweb/rewriter/public/javascript_library_identification.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/null_statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/js/js_tokenizer.h"

namespace net_instaweb {

extern const char* JS_console_js;

namespace {

void TestMinifyJavascript(benchmark::State& state) {
  GoogleString in_text;
  for (int i = 0; i < state.iterations(); i += strlen(JS_console_js)) {
    in_text += JS_console_js;
  }
  in_text.resize(state.iterations());

  NullStatistics stats;
  JavascriptRewriteConfig::InitStats(&stats);
  pagespeed::js::JsTokenizerPatterns js_tokenizer_patterns;
  JavascriptLibraryIdentification js_lib_id;
  JavascriptRewriteConfig config(&stats, true /* minify */, &js_lib_id,
                                 &js_tokenizer_patterns);

  NullMessageHandler handler;
  for (int i = 0; i < state.iterations(); ++i) {
    JavascriptCodeBlock block(in_text, &config, "" /* message_id */, &handler);
    block.Rewrite();
  }
}

static void BM_MinifyJavascript(benchmark::State& state) {
  TestMinifyJavascript(state);
}
BENCHMARK_RANGE(BM_MinifyJavascript, 1 << 6, 1 << 18);

}  // namespace

}  // namespace net_instaweb
