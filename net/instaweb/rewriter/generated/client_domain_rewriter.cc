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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/client_domain_rewriter_dbg.js

namespace net_instaweb {

const char* JS_client_domain_rewriter =
    "(function(){var ENTER_KEY_CODE = 13;\nwindow.pagespeed = wind"
    "ow.pagespeed || {};\nvar pagespeed = window.pagespeed;\npagesp"
    "eed.ClientDomainRewriter = function(a) {\n  this.mappedDomain"
    "Names_ = a;\n};\npagespeed.ClientDomainRewriter.prototype.anch"
    "orListener = function(a) {\n  a = a || window.event;\n  if (a."
    "type != \"keypress\" || a.keyCode == ENTER_KEY_CODE) {\n    for"
    " (var b = a.target; b != null; b = b.parentNode) {\n      if "
    "(b.tagName == \"A\") {\n        this.processEvent(b.href, a);\n "
    "       break;\n      }\n    }\n  }\n};\npagespeed.ClientDomainRew"
    "riter.prototype.addEventListeners = function() {\n  var a = t"
    "his;\n  document.body.onclick = function(b) {\n    a.anchorLis"
    "tener(b);\n  };\n  document.body.onkeypress = function(b) {\n  "
    "  a.anchorListener(b);\n  };\n};\npagespeed.ClientDomainRewrite"
    "r.prototype.processEvent = function(a, b) {\n  for (var c = 0"
    "; c < this.mappedDomainNames_.length; c++) {\n    if (a.index"
    "Of(this.mappedDomainNames_[c]) == 0) {\n      window.location"
    " = window.location.protocol + \"//\" + window.location.hostnam"
    "e + \"/\" + a.substr(this.mappedDomainNames_[c].length);\n     "
    " b.preventDefault();\n      break;\n    }\n  }\n};\npagespeed.cli"
    "entDomainRewriterInit = function(a) {\n  a = new pagespeed.Cl"
    "ientDomainRewriter(a);\n  pagespeed.clientDomainRewriter = a;"
    "\n  a.addEventListeners();\n};\npagespeed.clientDomainRewriterI"
    "nit = pagespeed.clientDomainRewriterInit;\n})();\n";

}  // namespace net_instaweb
