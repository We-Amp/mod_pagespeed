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

#ifndef PAGESPEED_KERNEL_BASE_ESCAPING_H_
#define PAGESPEED_KERNEL_BASE_ESCAPING_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Appends version of original escaped for JS string syntax, safe for inclusion
// into HTML, to *escaped, (optionally with quotes, if asked).
void EscapeToJsStringLiteral(const StringPiece& original, bool add_quotes,
                             GoogleString* escaped);

// Appends version of original escaped for JSON string syntax to *escaped,
// (optionally with quotes, if asked).
//
// Well-formed multi-byte UTF-8 sequences are copied verbatim, so the
// output decodes back to the same text. A byte that is not part of a
// well-formed UTF-8 sequence (a lone continuation byte, a truncated,
// overlong or surrogate sequence) is escaped as \u00xx, as are control
// characters and the JSON metacharacters, so the output is always valid
// JSON and has none of the characters that can cause format sniffing.
void EscapeToJsonStringLiteral(const StringPiece& original, bool add_quotes,
                               GoogleString* escaped);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_BASE_ESCAPING_H_
