/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/defer_iframe_dbg.js

namespace net_instaweb {

const char* JS_defer_iframe =
    "(function(){window.pagespeed = window.pagespeed || {};\nvar p"
    "agespeed = window.pagespeed;\npagespeed.DeferIframe = functio"
    "n() {\n};\npagespeed.DeferIframe.prototype.convertToIframe = f"
    "unction() {\n  var a = document.getElementsByTagName(\"pagespe"
    "ed_iframe\");\n  if (a.length > 0) {\n    a = a[0];\n    for (va"
    "r d = document.createElement(\"iframe\"), b = 0, c = a.attribu"
    "tes, e = c.length; b < e; ++b) {\n      d.setAttribute(c[b].n"
    "ame, c[b].value);\n    }\n    a.parentNode.replaceChild(d, a);"
    "\n  }\n};\npagespeed.DeferIframe.prototype.convertToIframe = pa"
    "gespeed.DeferIframe.prototype.convertToIframe;\npagespeed.def"
    "erIframeInit = function() {\n  var a = new pagespeed.DeferIfr"
    "ame();\n  pagespeed.deferIframe = a;\n};\npagespeed.deferIframe"
    "Init = pagespeed.deferIframeInit;\n})();\n";

}  // namespace net_instaweb
