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
// Automatically generated from critical_css_beacon_dbg.js

namespace net_instaweb {

const char* JS_critical_css_beacon =
    "(function(){var pagespeedutils = {MAX_POST_SIZE:131072, send"
    "Beacon:function(a, b, d) {\n  if (window.XMLHttpRequest) {\n  "
    "  var c = new XMLHttpRequest();\n  } else if (window.ActiveXO"
    "bject) {\n    try {\n      c = new ActiveXObject(\"Msxml2.XMLHT"
    "TP\");\n    } catch (e) {\n      try {\n        c = new ActiveXO"
    "bject(\"Microsoft.XMLHTTP\");\n      } catch (g) {\n      }\n    "
    "}\n  }\n  if (!c) {\n    return !1;\n  }\n  var f = a.indexOf(\"?\""
    ") == -1 ? \"?\" : \"&\";\n  a = a + f + \"url=\" + encodeURICompone"
    "nt(b);\n  c.open(\"POST\", a);\n  c.setRequestHeader(\"Content-Ty"
    "pe\", \"application/x-www-form-urlencoded\");\n  c.send(d);\n  re"
    "turn !0;\n}, addHandler:function(a, b, d) {\n  if (a.addEventL"
    "istener) {\n    a.addEventListener(b, d, !1);\n  } else if (a."
    "attachEvent) {\n    a.attachEvent(\"on\" + b, d);\n  } else {\n  "
    "  var c = a[\"on\" + b];\n    a[\"on\" + b] = function() {\n      "
    "d.call(this);\n      c && c.call(this);\n    };\n  }\n}, getPosi"
    "tion:function(a) {\n  for (var b = a.offsetTop, d = a.offsetL"
    "eft; a.offsetParent;) {\n    a = a.offsetParent, b += a.offse"
    "tTop, d += a.offsetLeft;\n  }\n  return {top:b, left:d};\n}, ge"
    "tWindowSize:function() {\n  return {height:window.innerHeight"
    " || document.documentElement.clientHeight || document.body.c"
    "lientHeight, width:window.innerWidth || document.documentEle"
    "ment.clientWidth || document.body.clientWidth};\n}, inViewpor"
    "t:function(a, b) {\n  a = pagespeedutils.getPosition(a);\n  re"
    "turn pagespeedutils.positionInViewport(a, b);\n}, positionInV"
    "iewport:function(a, b) {\n  return a.top < b.height && a.left"
    " < b.width;\n}, getRequestAnimationFrame:function() {\n  retur"
    "n window.requestAnimationFrame || window.webkitRequestAnimat"
    "ionFrame || window.mozRequestAnimationFrame || window.oReque"
    "stAnimationFrame || window.msRequestAnimationFrame || null;\n"
    "}};\npagespeedutils.now = Date.now || function() {\n  return +"
    "new Date();\n};\nwindow.pagespeed = window.pagespeed || {};\nva"
    "r pagespeed = window.pagespeed;\npagespeed.CriticalCssBeacon "
    "= function(a, b, d, c, f, e) {\n  this.MAXITERS_ = 250;\n  thi"
    "s.MAXMEASURES_ = 10;\n  this.beaconUrl_ = a;\n  this.htmlUrl_ "
    "= b;\n  this.optionsHash_ = d;\n  this.nonce_ = c;\n  this.sele"
    "ctors_ = f;\n  this.aboveTheFoldOnly_ = !!e;\n  this.criticalS"
    "electors_ = [];\n  this.idx_ = 0;\n};\npagespeed.CriticalCssBea"
    "con.prototype.sendBeacon_ = function() {\n  var a = \"oh=\" + t"
    "his.optionsHash_ + \"&n=\" + this.nonce_;\n  a += \"&cs=\";\n  for"
    " (var b = this.criticalSelectors_.length, d = Math.floor(Mat"
    "h.random() * b), c = pagespeedutils.MAX_POST_SIZE - 5, f = !"
    "1, e = 0; e < b; ++e) {\n    var g = e > 0 ? \",\" : \"\";\n    g "
    "+= encodeURIComponent(this.criticalSelectors_[(d + e) % b]);"
    "\n    if (a.length + g.length > c) {\n      f = !0;\n      brea"
    "k;\n    }\n    a += g;\n  }\n  f && (a += \"&of=1\");\n  pagespeed."
    "criticalCssBeaconData = a;\n  pagespeedutils.sendBeacon(this."
    "beaconUrl_, this.htmlUrl_, a);\n};\npagespeed.CriticalCssBeaco"
    "n.prototype.isSelectorCritical_ = function(a) {\n  a = docume"
    "nt.querySelectorAll(a);\n  if (a.length == 0) {\n    return !1"
    ";\n  }\n  if (!this.aboveTheFoldOnly_) {\n    return !0;\n  }\n  "
    "var b = window.innerHeight || document.documentElement.clien"
    "tHeight;\n  if (!b || !a[0].getBoundingClientRect) {\n    retu"
    "rn !0;\n  }\n  for (var d = window.pageYOffset || 0, c = Math."
    "min(a.length, this.MAXMEASURES_), f = 0; f < c; ++f) {\n    v"
    "ar e = a[f].getBoundingClientRect();\n    if (e.width == 0 &&"
    " e.height == 0 || e.top + d < b) {\n      return !0;\n    }\n  "
    "}\n  return a.length > c;\n};\npagespeed.CriticalCssBeacon.prot"
    "otype.checkCssSelectors_ = function(a) {\n  for (var b = 0; b"
    " < this.MAXITERS_ && this.idx_ < this.selectors_.length; ++b"
    ", ++this.idx_) {\n    try {\n      this.isSelectorCritical_(th"
    "is.selectors_[this.idx_]) && this.criticalSelectors_.push(th"
    "is.selectors_[this.idx_]);\n    } catch (d) {\n      this.abov"
    "eTheFoldOnly_ || this.criticalSelectors_.push(this.selectors"
    "_[this.idx_]);\n    }\n  }\n  this.idx_ < this.selectors_.lengt"
    "h ? window.setTimeout(this.checkCssSelectors_.bind(this), 0,"
    " a) : a();\n};\npagespeed.criticalCssBeaconInit = function(a, "
    "b, d, c, f, e) {\n  if (document.querySelector && document.qu"
    "erySelectorAll && Function.prototype.bind) {\n    var g = new"
    " pagespeed.CriticalCssBeacon(a, b, d, c, f, e);\n    pagespee"
    "dutils.addHandler(window, \"load\", function() {\n      window."
    "setTimeout(function() {\n        g.checkCssSelectors_(functio"
    "n() {\n          g.sendBeacon_();\n        });\n      }, 0);\n  "
    "  });\n  }\n};\npagespeed.criticalCssBeaconInit = pagespeed.cri"
    "ticalCssBeaconInit;\n})();\n";

}  // namespace net_instaweb
