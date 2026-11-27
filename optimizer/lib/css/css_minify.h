// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#ifndef PAGESPEED_LIB_CSS_CSS_MINIFY_H_
#define PAGESPEED_LIB_CSS_CSS_MINIFY_H_

#include <string>
#include <string_view>

namespace pagespeed {
namespace css {

// Minifies CSS by removing comments, collapsing whitespace, and
// removing unnecessary characters. Comments starting with /*! or
// containing "@license"/"@preserve" are kept byte for byte, in their
// position. Returns true on success.
// On any internal error, returns false and output is unchanged.
bool MinifyCss(std::string_view input, std::string* output);

}  // namespace css
}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CSS_CSS_MINIFY_H_
