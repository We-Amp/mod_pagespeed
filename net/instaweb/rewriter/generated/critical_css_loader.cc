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
// Automatically generated from critical_css_loader_dbg.js

namespace net_instaweb {

const char* JS_critical_css_loader =
    "(() => {\n  var addHandler = function(elem, eventName, func) "
    "{\n    if (elem.addEventListener) {\n      elem.addEventListen"
    "er(eventName, func, false);\n    } else if (elem.attachEvent)"
    " {\n      elem.attachEvent(\"on\" + eventName, func);\n    } els"
    "e {\n      var oldHandler = elem[\"on\" + eventName];\n      ele"
    "m[\"on\" + eventName] = function() {\n        func.call(this);\n"
    "        if (oldHandler) {\n          oldHandler.call(this);\n "
    "       }\n      };\n    }\n  };\n  var now = Date.now || (functi"
    "on() {\n    return +new Date();\n  });\n\n  window.pagespeed = w"
    "indow.pagespeed || {};\n  var pagespeed = window.pagespeed;\n "
    " pagespeed.CriticalCssLoader = pagespeed.CriticalCssLoader |"
    "| {};\n  pagespeed.CriticalCssLoader.DEFERRED_ATTRIBUTE_ = \"d"
    "ata-pagespeed-deferred-css\";\n  pagespeed.CriticalCssLoader.s"
    "tarted_ = false;\n  pagespeed.CriticalCssLoader.apply_ = func"
    "tion(link) {\n    if (link.getAttribute(\"rel\") == \"preload\") "
    "{\n      link.setAttribute(\"rel\", \"stylesheet\");\n    }\n  };\n "
    " pagespeed.CriticalCssLoader.onLinkEvent_ = function(event) "
    "{\n    var target = event.target;\n    if (target && target.no"
    "deName == \"LINK\" && target.hasAttribute(pagespeed.CriticalCs"
    "sLoader.DEFERRED_ATTRIBUTE_)) {\n      pagespeed.CriticalCssL"
    "oader.apply_(target);\n    }\n  };\n  pagespeed.CriticalCssLoad"
    "er.applyAll = function() {\n    var links = document.querySel"
    "ectorAll(\n      \"link[\" + pagespeed.CriticalCssLoader.DEFERR"
    "ED_ATTRIBUTE_ + \"]\"\n    );\n    for (var i = 0; i < links.len"
    "gth; ++i) {\n      pagespeed.CriticalCssLoader.apply_(links[i"
    "]);\n    }\n  };\n  pagespeed.CriticalCssLoader.Run = function("
    ") {\n    if (pagespeed.CriticalCssLoader.started_) {\n      re"
    "turn;\n    }\n    pagespeed.CriticalCssLoader.started_ = true;"
    "\n    document.addEventListener(\n      \"load\",\n      pagespee"
    "d.CriticalCssLoader.onLinkEvent_,\n      true\n    );\n    docu"
    "ment.addEventListener(\n      \"error\",\n      pagespeed.Critic"
    "alCssLoader.onLinkEvent_,\n      true\n    );\n    var probe = "
    "document.createElement(\"link\");\n    var supportsPreload = !!"
    "(probe.relList && probe.relList.supports && probe.relList.su"
    "pports(\"preload\"));\n    if (!supportsPreload) {\n      addHan"
    "dler(\n        document,\n        \"DOMContentLoaded\",\n        "
    "pagespeed.CriticalCssLoader.applyAll\n      );\n    }\n    addH"
    "andler(\n      window,\n      \"load\",\n      pagespeed.Critical"
    "CssLoader.applyAll\n    );\n    pagespeed.CriticalCssLoader.ap"
    "plyAll();\n  };\n})();\n";

}  // namespace net_instaweb
