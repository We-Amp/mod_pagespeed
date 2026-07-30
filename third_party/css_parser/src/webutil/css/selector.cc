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

#include "third_party/css_parser/src/webutil/css/selector.h"

#include "third_party/css_parser/src/util/gtl/stl_util.h"

namespace Css {

//
// static
//

const HtmlTagIndex SimpleSelector::tagindex_;

//
// SimpleSelector factory methods
//

SimpleSelector* SimpleSelector::NewElementType(const UnicodeText& name) {
  HtmlTagEnum tag = static_cast<HtmlTagEnum>(
      tagindex_.FindHtmlTag(name.utf8_data(), name.utf8_length()));
  return new SimpleSelector(tag, name);
}

SimpleSelector* SimpleSelector::NewUniversal() {
  return new SimpleSelector(SimpleSelector::UNIVERSAL, UnicodeText(),
                            UnicodeText());
}

SimpleSelector* SimpleSelector::NewExistAttribute(
    const UnicodeText& attribute) {
  return new SimpleSelector(SimpleSelector::EXIST_ATTRIBUTE, attribute,
                            UnicodeText());
}

SimpleSelector* SimpleSelector::NewBinaryAttribute(Type type,
                                                   const UnicodeText& attribute,
                                                   const UnicodeText& value) {
  return new SimpleSelector(type, attribute, value);
}

static const char kClassText[] = "class";
SimpleSelector* SimpleSelector::NewClass(const UnicodeText& classname) {
  // Immortal/leaked static: never destroyed, so a rewrite worker
  // thread still parsing CSS during process exit cannot read a freed
  // UnicodeText. Plain function-local statics are torn down by
  // __run_exit_handlers on the main thread while worker threads are still
  // running (the same worker-vs-static-destruction shutdown race as the spdlog
  // logger; this CSS-parser sibling was found by the shutdown stress harness).
  static const UnicodeText& kClass =
      *new UnicodeText(UTF8ToUnicodeText(kClassText, strlen(kClassText)));
  return new SimpleSelector(SimpleSelector::CLASS, kClass, classname);
}

static const char kIdText[] = "id";
SimpleSelector* SimpleSelector::NewId(const UnicodeText& id) {
  // Immortal/leaked static — see NewClass above.
  static const UnicodeText& kId =
      *new UnicodeText(UTF8ToUnicodeText(kIdText, strlen(kIdText)));
  return new SimpleSelector(SimpleSelector::ID, kId, id);
}

// sep is the separator. Either ":" or "::".
// See: http://www.w3.org/TR/CSS2/selector.html#pseudo-elements
//  and http://www.w3.org/TR/css3-selectors/#pseudo-elements
SimpleSelector* SimpleSelector::NewPseudoclass(const UnicodeText& pseudoclass,
                                               const UnicodeText& sep) {
  return new SimpleSelector(SimpleSelector::PSEUDOCLASS, sep, pseudoclass);
}

SimpleSelector* SimpleSelector::NewFunctionalPseudoclass(
    const UnicodeText& pseudoclass, const UnicodeText& sep,
    const UnicodeText& function_arguments) {
  // Functional pseudo-class argument pass-through: the argument
  // text between the parens is stored verbatim and re-emitted on
  // serialization, rather than parsed into selector nodes — no mpp consumer
  // matches selectors against a DOM, so the arguments only need to survive
  // the round trip un-mangled.
  return new SimpleSelector(SimpleSelector::PSEUDOCLASS, sep, pseudoclass,
                            function_arguments);
}

SimpleSelector* SimpleSelector::NewLang(const UnicodeText& lang) {
  return new SimpleSelector(SimpleSelector::LANG, UnicodeText(), lang);
}

//
// Some destructors that need STLDeleteElements() from stl_util.h
//

SimpleSelectors::~SimpleSelectors() { STLDeleteElements(this); }
Selector::~Selector() { STLDeleteElements(this); }
Selectors::~Selectors() { STLDeleteElements(this); }

}  // namespace Css
