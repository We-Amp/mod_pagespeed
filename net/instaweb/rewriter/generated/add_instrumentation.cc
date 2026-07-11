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
// Automatically generated from add_instrumentation_dbg.js

namespace net_instaweb {

const char* JS_add_instrumentation =
    "(function(){window.pagespeed = window.pagespeed || {};\nvar p"
    "agespeed = window.pagespeed;\npagespeed.AddInstrumentation = "
    "function(a, c, b, d) {\n  this.beaconUrlPrefix_ = a;\n  this.e"
    "vent_ = c;\n  this.extraParams_ = b;\n  this.htmlUrl_ = d;\n};\n"
    "pagespeed.beaconUrl = \"\";\npagespeed.AddInstrumentation.proto"
    "type.sendBeacon = function() {\n  var a = this.beaconUrlPrefi"
    "x_, c = window.mod_pagespeed_start, b = Number(new Date()) -"
    " c;\n  a += a.indexOf(\"?\") == -1 ? \"?\" : \"&\";\n  a = a + \"ets="
    "\" + (this.event_ == \"load\" ? \"load:\" : \"unload:\");\n  if (thi"
    "s.event_ != \"beforeunload\" || !window.mod_pagespeed_loaded) "
    "{\n    a = a + b + (\"&r\" + this.event_ + \"=\");\n    if (window"
    ".performance) {\n      b = window.performance.timing;\n      v"
    "ar d = b.navigationStart, e = b.requestStart;\n      a += b[t"
    "his.event_ + \"EventStart\"] - d;\n      a += \"&nav=\" + (b.fetc"
    "hStart - d);\n      a += \"&dns=\" + (b.domainLookupEnd - b.dom"
    "ainLookupStart);\n      a += \"&connect=\" + (b.connectEnd - b."
    "connectStart);\n      a = a + (\"&req_start=\" + (e - d)) + (\"&"
    "ttfb=\" + (b.responseStart - e));\n      a += \"&dwld=\" + (b.re"
    "sponseEnd - b.responseStart);\n      a += \"&dom_c=\" + (b.domC"
    "ontentLoadedEventStart - d);\n      window.performance.naviga"
    "tion && (a += \"&nt=\" + window.performance.navigation.type);\n"
    "      d = -1;\n      b.msFirstPaint ? d = b.msFirstPaint : wi"
    "ndow.chrome && window.chrome.loadTimes && (d = Math.floor(wi"
    "ndow.chrome.loadTimes().firstPaintTime * 1000));\n      d -= "
    "e;\n      d >= 0 && (a += \"&fp=\" + d);\n    } else {\n      a +"
    "= b;\n    }\n    pagespeed.getResourceTimingData && window.par"
    "ent == window && (a += pagespeed.getResourceTimingData());\n "
    "   a += window.parent != window ? \"&ifr=1\" : \"&ifr=0\";\n    t"
    "his.event_ == \"load\" && (window.mod_pagespeed_loaded = !0, ("
    "b = window.mod_pagespeed_num_resources_prefetched) && (a += "
    "\"&nrp=\" + b), (b = window.mod_pagespeed_prefetch_start) && ("
    "a += \"&htmlAt=\" + (c - b)));\n    pagespeed.criticalCss && (c"
    " = pagespeed.criticalCss, a += \"&ccis=\" + c.total_critical_i"
    "nlined_size + \"&cces=\" + c.total_original_external_size + \"&"
    "ccos=\" + c.total_overhead_size + \"&ccrl=\" + c.num_replaced_l"
    "inks + \"&ccul=\" + c.num_unreplaced_links);\n    a += \"&dpr=\" "
    "+ window.devicePixelRatio;\n    this.extraParams_ != \"\" && (a"
    " += this.extraParams_);\n    document.referrer && (a += \"&ref"
    "=\" + encodeURIComponent(document.referrer));\n    a += \"&url="
    "\" + encodeURIComponent(this.htmlUrl_);\n    pagespeed.beaconU"
    "rl = a;\n    (new Image()).src = a;\n  }\n};\npagespeed.addInstr"
    "umentationInit = function(a, c, b, d) {\n  var e = new pagesp"
    "eed.AddInstrumentation(a, c, b, d);\n  window.addEventListene"
    "r ? window.addEventListener(c, function() {\n    e.sendBeacon"
    "();\n  }, !1) : window.attachEvent(\"on\" + c, function() {\n   "
    " e.sendBeacon();\n  });\n};\npagespeed.addInstrumentationInit ="
    " pagespeed.addInstrumentationInit;\n})();\n";

}  // namespace net_instaweb
