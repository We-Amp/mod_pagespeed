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
// Automatically generated from client_domain_rewriter_dbg.js

namespace net_instaweb {

const char* JS_client_domain_rewriter =
    "(() => {\n  var ENTER_KEY_CODE = 13;\n  window.pagespeed = win"
    "dow.pagespeed || {};\n  var pagespeed = window.pagespeed;\n  p"
    "agespeed.ClientDomainRewriter = function(mappedDomainNames) "
    "{\n    this.mappedDomainNames_ = mappedDomainNames;\n  };\n  pa"
    "gespeed.ClientDomainRewriter.prototype.anchorListener = func"
    "tion(event) {\n    event = event || window.event;\n    if (eve"
    "nt.type == \"keypress\" && event.keyCode != ENTER_KEY_CODE) {\n"
    "      return;\n    }\n    for (var target = event.target; targ"
    "et != null; target = target.parentNode) {\n      if (target.t"
    "agName == \"A\") {\n        this.processEvent(target.href, even"
    "t);\n        return;\n      }\n    }\n  };\n  pagespeed.ClientDom"
    "ainRewriter.prototype.addEventListeners = function() {\n    v"
    "ar me = this;\n    document.body.onclick = function(event) {\n"
    "      me.anchorListener(event);\n    };\n    document.body.onk"
    "eypress = function(event) {\n      me.anchorListener(event);\n"
    "    };\n  };\n  pagespeed.ClientDomainRewriter.prototype.proce"
    "ssEvent = function(url, event) {\n    for (var i = 0; i < thi"
    "s.mappedDomainNames_.length; i++) {\n      if (url.indexOf(th"
    "is.mappedDomainNames_[i]) == 0) {\n        window.location = "
    "window.location.protocol + \"//\" + window.location.hostname +"
    " \"/\" + url.substr(this.mappedDomainNames_[i].length);\n      "
    "  event.preventDefault();\n        return;\n      }\n    }\n  };"
    "\n  pagespeed.clientDomainRewriterInit = function(mappedDomai"
    "nNames) {\n    var clientDomainRewriter = new pagespeed.Clien"
    "tDomainRewriter(\n      mappedDomainNames\n    );\n    pagespee"
    "d.clientDomainRewriter = clientDomainRewriter;\n    clientDom"
    "ainRewriter.addEventListeners();\n  };\n})();\n";

}  // namespace net_instaweb
