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

#ifndef WEBUTIL_CSS_TOSTRING_H_
#define WEBUTIL_CSS_TOSTRING_H_

#include "third_party/css_parser/src/strings/stringpiece.h"
#include "third_party/css_parser/src/webutil/css/string.h"

class UnicodeText;

namespace Css {

// Escape text so that it is safe to put in "" in a CSS string.
string EscapeString(CssStringPiece src);
string EscapeString(const UnicodeText& src);
// Escape a text URL so that it is safe to put in url() in CSS.
string EscapeUrl(CssStringPiece src);
string EscapeUrl(const UnicodeText& src);
// Escape an identifier so that it will be re-parsed correctly.
string EscapeIdentifier(CssStringPiece src);
string EscapeIdentifier(const UnicodeText& src);
// Like EscapeIdentifier, for a name that must be a complete CSS identifier,
// such as a class or id name in a selector: a leading digit, or a digit after
// a leading '-', is written as a hex escape (CSS Syntax 3, section 4.3.9).
// EscapeIdentifier leaves those digits as they are, because it also writes
// names the parser accepts that are not identifiers in the source.
string EscapeIdentifierName(CssStringPiece src);
string EscapeIdentifierName(const UnicodeText& src);

}  // namespace Css

#endif  // WEBUTIL_CSS_TOSTRING_H_
