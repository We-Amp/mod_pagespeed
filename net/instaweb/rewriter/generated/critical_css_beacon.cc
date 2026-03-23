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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/critical_css_beacon_dbg.js

namespace net_instaweb {

const char* JS_critical_css_beacon =
    "(function(){var pagespeedutils = {MAX_POST_SIZE:131072, send"
    "Beacon:function(a, b, c) {\n  if (window.XMLHttpRequest) {\n  "
    "  var d = new XMLHttpRequest();\n  } else if (window.ActiveXO"
    "bject) {\n    try {\n      d = new ActiveXObject(\"Msxml2.XMLHT"
    "TP\");\n    } catch (f) {\n      try {\n        d = new ActiveXO"
    "bject(\"Microsoft.XMLHTTP\");\n      } catch (g) {\n      }\n    "
    "}\n  }\n  if (!d) {\n    return !1;\n  }\n  var e = a.indexOf(\"?\""
    ") == -1 ? \"?\" : \"&\";\n  a = a + e + \"url=\" + encodeURICompone"
    "nt(b);\n  d.open(\"POST\", a);\n  d.setRequestHeader(\"Content-Ty"
    "pe\", \"application/x-www-form-urlencoded\");\n  d.send(c);\n  re"
    "turn !0;\n}, addHandler:function(a, b, c) {\n  if (a.addEventL"
    "istener) {\n    a.addEventListener(b, c, !1);\n  } else if (a."
    "attachEvent) {\n    a.attachEvent(\"on\" + b, c);\n  } else {\n  "
    "  var d = a[\"on\" + b];\n    a[\"on\" + b] = function() {\n      "
    "c.call(this);\n      d && d.call(this);\n    };\n  }\n}, getPosi"
    "tion:function(a) {\n  for (var b = a.offsetTop, c = a.offsetL"
    "eft; a.offsetParent;) {\n    a = a.offsetParent, b += a.offse"
    "tTop, c += a.offsetLeft;\n  }\n  return {top:b, left:c};\n}, ge"
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
    "= function(a, b, c, d, e) {\n  this.MAXITERS_ = 250;\n  this.b"
    "eaconUrl_ = a;\n  this.htmlUrl_ = b;\n  this.optionsHash_ = c;"
    "\n  this.nonce_ = d;\n  this.selectors_ = e;\n  this.criticalSe"
    "lectors_ = [];\n  this.idx_ = 0;\n};\npagespeed.CriticalCssBeac"
    "on.prototype.sendBeacon_ = function() {\n  var a = \"oh=\" + th"
    "is.optionsHash_ + \"&n=\" + this.nonce_;\n  a += \"&cs=\";\n  for "
    "(var b = 0; b < this.criticalSelectors_.length; ++b) {\n    v"
    "ar c = b > 0 ? \",\" : \"\";\n    c += encodeURIComponent(this.cr"
    "iticalSelectors_[b]);\n    if (a.length + c.length > pagespee"
    "dutils.MAX_POST_SIZE) {\n      break;\n    }\n    a += c;\n  }\n "
    " pagespeed.criticalCssBeaconData = a;\n  pagespeedutils.sendB"
    "eacon(this.beaconUrl_, this.htmlUrl_, a);\n};\npagespeed.Criti"
    "calCssBeacon.prototype.checkCssSelectors_ = function(a) {\n  "
    "for (var b = 0; b < this.MAXITERS_ && this.idx_ < this.selec"
    "tors_.length; ++b, ++this.idx_) {\n    try {\n      document.q"
    "uerySelector(this.selectors_[this.idx_]) != null && this.cri"
    "ticalSelectors_.push(this.selectors_[this.idx_]);\n    } catc"
    "h (c) {\n    }\n  }\n  this.idx_ < this.selectors_.length ? win"
    "dow.setTimeout(this.checkCssSelectors_.bind(this), 0, a) : a"
    "();\n};\npagespeed.criticalCssBeaconInit = function(a, b, c, d"
    ", e) {\n  if (document.querySelector && Function.prototype.bi"
    "nd) {\n    var f = new pagespeed.CriticalCssBeacon(a, b, c, d"
    ", e);\n    pagespeedutils.addHandler(window, \"load\", function"
    "() {\n      window.setTimeout(function() {\n        f.checkCss"
    "Selectors_(function() {\n          f.sendBeacon_();\n        }"
    ");\n      }, 0);\n    });\n  }\n};\npagespeed.criticalCssBeaconIn"
    "it = pagespeed.criticalCssBeaconInit;\n})();\n";

}  // namespace net_instaweb
