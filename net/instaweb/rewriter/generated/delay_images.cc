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
// Automatically generated from delay_images_dbg.js

namespace net_instaweb {

const char* JS_delay_images =
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
    "r pagespeed = window.pagespeed;\npagespeed.DelayImages = func"
    "tion() {\n  this.highResReplaced_ = this.lazyLoadHighResHandl"
    "ersRegistered_ = !1;\n};\npagespeed.DelayImages.prototype.repl"
    "aceElementSrc = function(a) {\n  for (var b = 0; b < a.length"
    "; ++b) {\n    var c = a[b].getAttribute(\"data-pagespeed-high-"
    "res-src\");\n    c && a[b].setAttribute(\"src\", c);\n  }\n};\npage"
    "speed.DelayImages.prototype.replaceElementSrc = pagespeed.De"
    "layImages.prototype.replaceElementSrc;\npagespeed.DelayImages"
    ".prototype.registerLazyLoadHighRes = function() {\n  if (this"
    ".lazyLoadHighResHandlersRegistered_) {\n    this.highResRepla"
    "ced_ = !1;\n  } else {\n    var a = document.body, b, c = 0, d"
    " = this;\n    this.highResReplaced = !1;\n    \"ontouchstart\" i"
    "n a ? (pagespeedutils.addHandler(a, \"touchstart\", function(e"
    ") {\n      b = pagespeedutils.now();\n    }), pagespeedutils.a"
    "ddHandler(a, \"touchend\", function(e) {\n      c = pagespeedut"
    "ils.now();\n      (e.changedTouches != null && e.changedTouch"
    "es.length == 2 || e.touches != null && e.touches.length == 2"
    " || c - b < 500) && d.loadHighRes();\n    })) : pagespeedutil"
    "s.addHandler(window, \"click\", function(e) {\n      d.loadHigh"
    "Res();\n    });\n    pagespeedutils.addHandler(window, \"load\","
    " function(e) {\n      d.loadHighRes();\n    });\n    this.lazyL"
    "oadHighResHandlersRegistered_ = !0;\n  }\n};\npagespeed.DelayIm"
    "ages.prototype.registerLazyLoadHighRes = pagespeed.DelayImag"
    "es.prototype.registerLazyLoadHighRes;\npagespeed.DelayImages."
    "prototype.loadHighRes = function() {\n  this.highResReplaced_"
    " || (this.replaceWithHighRes(), this.highResReplaced_ = !0);"
    "\n};\npagespeed.DelayImages.prototype.replaceWithHighRes = fun"
    "ction() {\n  this.replaceElementSrc(document.getElementsByTag"
    "Name(\"img\"));\n  this.replaceElementSrc(document.getElementsB"
    "yTagName(\"input\"));\n};\npagespeed.DelayImages.prototype.repla"
    "ceWithHighRes = pagespeed.DelayImages.prototype.replaceWithH"
    "ighRes;\npagespeed.delayImagesInit = function() {\n  var a = n"
    "ew pagespeed.DelayImages();\n  pagespeed.delayImages = a;\n};\n"
    "pagespeed.delayImagesInit = pagespeed.delayImagesInit;\n})();"
    "\n";

}  // namespace net_instaweb
