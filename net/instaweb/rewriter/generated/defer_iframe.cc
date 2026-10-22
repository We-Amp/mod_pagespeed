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
// Automatically generated from defer_iframe_dbg.js

namespace net_instaweb {

const char* JS_defer_iframe =
    "(() => {\n  window.pagespeed = window.pagespeed || {};\n  var "
    "pagespeed = window.pagespeed;\n  pagespeed.DeferIframe = func"
    "tion() {\n  };\n  pagespeed.DeferIframe.prototype.convertToIfr"
    "ame = function() {\n    var nodes = document.getElementsByTag"
    "Name(\"pagespeed_iframe\");\n    if (nodes.length > 0) {\n      "
    "var oldElement = nodes[0];\n      var newElement = document.c"
    "reateElement(\"iframe\");\n      for (var i = 0, a = oldElement"
    ".attributes, n = a.length; i < n; ++i) {\n        newElement."
    "setAttribute(a[i].name, a[i].value);\n      }\n      oldElemen"
    "t.parentNode.replaceChild(newElement, oldElement);\n    }\n  }"
    ";\n  pagespeed.deferIframeInit = function() {\n    var deferIf"
    "rame = new pagespeed.DeferIframe();\n    pagespeed.deferIfram"
    "e = deferIframe;\n  };\n})();\n";

}  // namespace net_instaweb
