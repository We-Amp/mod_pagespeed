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
// Shared construction of the WORST-CASE option-context payload.
//
// This lives in its own header because the number it produces is only
// meaningful when measured against the largest options class that actually
// SHIPS, and that class is not visible from the test target where the rest of
// the option-context tests live. Every server flavour subclasses RewriteOptions
// and registers its own properties -- SystemRewriteOptions adds ~51 and
// ApacheConfig ~3 more, and they are exactly the long-named infrastructure
// options whose records are the largest in the table. A worst case measured on
// base RewriteOptions alone is roughly three quarters of the real one, and it
// reads as comfortable while being wrong.
//
// So the measurement is written once here and instantiated from two different
// TEST BINARIES. That framing is deliberate, because the obvious one is wrong:
// property registration is process-static, so once ApacheConfig::Initialize()
// has run, every options instance in that binary reports the full table --
// comparing the classes to each other inside one binary measures nothing.
// What differs is the BINARY: the rewriter tests link no server flavour and see
// the base table, the Apache tests see base + system + Apache. The bound is
// derived from the larger, and the smaller keeps a base-table explosion visible
// on legs that do not link a server flavour.

#ifndef NET_INSTAWEB_REWRITER_OPTION_CONTEXT_WORST_CASE_H_
#define NET_INSTAWEB_REWRITER_OPTION_CONTEXT_WORST_CASE_H_

#include <cstddef>

#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

// Drives `options` to the largest option context it can express -- every filter
// enabled, every registered option explicitly set -- then serializes it and
// asserts the result fits inside the bound with the reserve still unspent.
//
// `class_name` names the options class in the failure message, because "the
// worst case grew" is only actionable if you know which class grew.
//
// `*out_payload_size` receives the measured size so a caller can report it.
inline void AssertWorstCaseOptionContextFitsTheBound(RewriteOptions* options,
                                                     const char* class_name,
                                                     size_t* out_payload_size) {
  for (int i = RewriteOptions::kFirstFilter; i != RewriteOptions::kEndOfFilters;
       ++i) {
    options->EnableFilter(static_cast<RewriteOptions::Filter>(i));
  }

  const RewriteOptions::OptionBaseVector& all = options->all_options();
  ASSERT_FALSE(all.empty()) << class_name << " registered no options at all";

  size_t settable = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    // Re-setting each option to its CURRENT value flips was_set() without
    // inventing values that could not occur in the field.
    //
    // The return is checked, not discarded. A type whose SetFromString cannot
    // round-trip its own ToString silently drops that option out of the "worst
    // case" -- which would make this measurement quietly smaller than the real
    // one, i.e. exactly the failure this whole test exists to prevent. The
    // ones that legitimately cannot round-trip (composite value types) are
    // counted rather than asserted away, and the count is asserted to be a
    // small minority so that a regression turning many options unsettable
    // cannot pass unnoticed.
    GoogleString error;
    if (all[i]->SetFromString(all[i]->ToString(), &error)) {
      ++settable;
    }
  }
  EXPECT_GT(settable * 2, all.size())
      << class_name << ": only " << settable << " of " << all.size()
      << " options round-tripped through SetFromString(ToString()), so this "
         "measurement is not the worst case it claims to be";

  GoogleString payload;
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Serialize(*options, &payload));
  *out_payload_size = payload.size();

  EXPECT_LE(payload.size(), kMaxOptionContextBytes - kOptionContextSizeReserve)
      << class_name << ": the worst-case option context is " << payload.size()
      << " bytes, which has grown into the " << kOptionContextSizeReserve
      << "-byte reserve under the " << kMaxOptionContextBytes
      << "-byte bound. Raise the bound DELIBERATELY rather than shrinking the "
         "reserve -- and note the bound is shared: it must move in the "
         "consuming component and on the `# bound:` line of the shared "
         "golden-vector file at the same time, or the two sides disagree "
         "about what they will accept.";
}

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_OPTION_CONTEXT_WORST_CASE_H_
