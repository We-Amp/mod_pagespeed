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
    "function(a, c, b) {\n  this.beaconUrlPrefix_ = a;\n  this.extr"
    "aParams_ = c;\n  this.htmlUrl_ = b;\n  this.sent_ = !1;\n  this"
    ".lcpMs_ = -1;\n  this.clsSession_ = {sum:0, firstTime:0, last"
    "Time:0};\n  this.cls_ = 0;\n  this.clsObserved_ = !1;\n  this.i"
    "npMs_ = -1;\n  this.observers_ = [];\n  this.initObservers_();"
    "\n};\npagespeed.AddInstrumentation.prototype.observe_ = functi"
    "on(a, c, b) {\n  var e = window.PerformanceObserver;\n  if (!e"
    " || !e.supportedEntryTypes || e.supportedEntryTypes.indexOf("
    "a) == -1) {\n    return !1;\n  }\n  try {\n    var d = new e(fun"
    "ction(f) {\n      c(f.getEntries());\n    });\n    b = b || {};"
    "\n    b.type = a;\n    b.buffered = !0;\n    d.observe(b);\n    "
    "this.observers_.push({observer:d, handler:c});\n    return !0"
    ";\n  } catch (f) {\n    return !1;\n  }\n};\npagespeed.AddInstrum"
    "entation.prototype.initObservers_ = function() {\n  var a = t"
    "his;\n  this.observe_(\"largest-contentful-paint\", function(b)"
    " {\n    for (var e = 0; e < b.length; e++) {\n      var d = b["
    "e];\n      a.lcpMs_ = Math.round(d.renderTime || d.loadTime |"
    "| d.startTime);\n    }\n  });\n  this.clsObserved_ = this.obser"
    "ve_(\"layout-shift\", function(b) {\n    for (var e = 0; e < b."
    "length; e++) {\n      var d = b[e];\n      if (!d.hadRecentInp"
    "ut) {\n        var f = a.clsSession_;\n        f.sum > 0 && d."
    "startTime - f.lastTime < 1000 && d.startTime - f.firstTime <"
    " 5000 ? (f.sum += d.value, f.lastTime = d.startTime) : (f.su"
    "m = d.value, f.firstTime = f.lastTime = d.startTime);\n      "
    "  f.sum > a.cls_ && (a.cls_ = f.sum);\n      }\n    }\n  });\n  "
    "var c = function(b) {\n    for (var e = 0; e < b.length; e++)"
    " {\n      var d = b[e];\n      if (d.entryType != \"event\" || d"
    ".interactionId) {\n        d = Math.round(d.duration), d > a."
    "inpMs_ && (a.inpMs_ = d);\n      }\n    }\n  };\n  this.observe_"
    "(\"event\", c, {durationThreshold:40});\n  this.observe_(\"first"
    "-input\", c);\n};\npagespeed.AddInstrumentation.prototype.rearm"
    "_ = function() {\n  for (var a = 0; a < this.observers_.lengt"
    "h; a++) {\n    try {\n      this.observers_[a].observer.discon"
    "nect();\n    } catch (c) {\n    }\n  }\n  this.observers_ = [];\n"
    "  this.sent_ = !1;\n  this.lcpMs_ = -1;\n  this.clsSession_ = "
    "{sum:0, firstTime:0, lastTime:0};\n  this.cls_ = 0;\n  this.in"
    "pMs_ = -1;\n  this.initObservers_();\n};\npagespeed.AddInstrume"
    "ntation.prototype.buildBody_ = function() {\n  var a = [], c "
    "= window.performance, b = null;\n  c && c.getEntriesByType &&"
    " (c = c.getEntriesByType(\"navigation\")) && c.length > 0 && ("
    "b = c[0]);\n  b ? (c = Math.round(b.loadEventStart), c > 0 &&"
    " (a.push(\"ets=load:\" + c), a.push(\"rload=\" + c)), a.push(\"na"
    "v=\" + Math.round(b.fetchStart)), a.push(\"dns=\" + Math.round("
    "b.domainLookupEnd - b.domainLookupStart)), a.push(\"connect=\""
    " + Math.round(b.connectEnd - b.connectStart)), a.push(\"req_s"
    "tart=\" + Math.round(b.requestStart)), a.push(\"c_ttfb=\" + Mat"
    "h.round(b.responseStart)), a.push(\"dwld=\" + Math.round(b.res"
    "ponseEnd - b.responseStart)), a.push(\"dom_c=\" + Math.round(b"
    ".domContentLoadedEventStart)), b.type && \n  a.push(\"nt=\" + b"
    ".type)) : window.mod_pagespeed_start && a.push(\"ets=load:\" +"
    " (Number(new Date()) - window.mod_pagespeed_start));\n  this."
    "lcpMs_ >= 0 && a.push(\"lcp=\" + this.lcpMs_);\n  this.clsObser"
    "ved_ && a.push(\"cls=\" + Math.round(this.cls_ * 1000));\n  thi"
    "s.inpMs_ >= 0 && a.push(\"inp=\" + this.inpMs_);\n  a.push(\"dpr"
    "=\" + window.devicePixelRatio);\n  a = a.join(\"&\");\n  this.ext"
    "raParams_ != \"\" && (a += this.extraParams_);\n  pagespeed.get"
    "ResourceTimingData && (a += pagespeed.getResourceTimingData("
    "));\n  document.referrer && (a += \"&ref=\" + encodeURIComponen"
    "t(document.referrer));\n  return a;\n};\npagespeed.AddInstrumen"
    "tation.prototype.send_ = function() {\n  if (!this.sent_) {\n "
    "   this.sent_ = !0;\n    for (var a = 0; a < this.observers_."
    "length; a++) {\n      var c = this.observers_[a];\n      try {"
    "\n        c.handler(c.observer.takeRecords()), c.observer.dis"
    "connect();\n      } catch (e) {\n      }\n    }\n    a = this.bu"
    "ildBody_();\n    c = this.beaconUrlPrefix_;\n    c += c.indexO"
    "f(\"?\") == -1 ? \"?\" : \"&\";\n    c += \"url=\" + encodeURICompone"
    "nt(this.htmlUrl_);\n    var b = window.navigator;\n    if (b &"
    "& b.sendBeacon) {\n      try {\n        if (b.sendBeacon(c, a)"
    ") {\n          return;\n        }\n      } catch (e) {\n      }\n"
    "    }\n    (new Image()).src = c + \"&\" + a;\n  }\n};\npagespeed."
    "addInstrumentationInit = function(a, c, b) {\n  if (window.pa"
    "rent == window) {\n    var e = new pagespeed.AddInstrumentati"
    "on(a, c, b);\n    document.addEventListener(\"visibilitychange"
    "\", function() {\n      document.visibilityState == \"hidden\" &"
    "& e.send_();\n    });\n    window.addEventListener(\"pagehide\","
    " function() {\n      e.send_();\n    });\n    window.addEventLi"
    "stener(\"pageshow\", function(d) {\n      d.persisted && e.rear"
    "m_();\n    });\n  }\n};\npagespeed.addInstrumentationInit = page"
    "speed.addInstrumentationInit;\n})();\n";

}  // namespace net_instaweb
