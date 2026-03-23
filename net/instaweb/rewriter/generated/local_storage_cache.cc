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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/local_storage_cache_dbg.js

namespace net_instaweb {

const char* JS_local_storage_cache =
    "(function(){var pagespeedutils = {MAX_POST_SIZE:131072, send"
    "Beacon:function(a, b, c) {\n  if (window.XMLHttpRequest) {\n  "
    "  var d = new XMLHttpRequest();\n  } else if (window.ActiveXO"
    "bject) {\n    try {\n      d = new ActiveXObject(\"Msxml2.XMLHT"
    "TP\");\n    } catch (e) {\n      try {\n        d = new ActiveXO"
    "bject(\"Microsoft.XMLHTTP\");\n      } catch (h) {\n      }\n    "
    "}\n  }\n  if (!d) {\n    return !1;\n  }\n  var f = a.indexOf(\"?\""
    ") == -1 ? \"?\" : \"&\";\n  a = a + f + \"url=\" + encodeURICompone"
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
    "r pagespeed = window.pagespeed;\npagespeed.LocalStorageCache "
    "= function() {\n  this.regenerate_cookie_ = !0;\n};\npagespeed."
    "LocalStorageCache.prototype.hasExpired = function(a) {\n  a ="
    " parseInt(a.substring(0, a.indexOf(\" \")), 10);\n  return !isN"
    "aN(a) && a <= pagespeedutils.now();\n};\npagespeed.LocalStorag"
    "eCache.prototype.hasExpired = pagespeed.LocalStorageCache.pr"
    "ototype.hasExpired;\npagespeed.LocalStorageCache.prototype.ge"
    "tData = function(a) {\n  var b = a.indexOf(\" \");\n  b = a.inde"
    "xOf(\" \", b + 1);\n  return a.substring(b + 1);\n};\npagespeed.L"
    "ocalStorageCache.prototype.getData = pagespeed.LocalStorageC"
    "ache.prototype.getData;\npagespeed.LocalStorageCache.prototyp"
    "e.replaceLastScript = function(a) {\n  var b = document.getEl"
    "ementsByTagName(\"script\");\n  b = b[b.length - 1];\n  b.parent"
    "Node.replaceChild(a, b);\n};\npagespeed.LocalStorageCache.prot"
    "otype.replaceLastScript = pagespeed.LocalStorageCache.protot"
    "ype.replaceLastScript;\npagespeed.LocalStorageCache.prototype"
    ".inlineCss = function(a) {\n  var b = window.localStorage.get"
    "Item(\"pagespeed_lsc_url:\" + a), c = document.createElement(b"
    " ? \"style\" : \"link\");\n  b && !this.hasExpired(b) ? (c.type ="
    " \"text/css\", c.appendChild(document.createTextNode(this.getD"
    "ata(b)))) : (c.rel = \"stylesheet\", c.href = a, this.regenera"
    "te_cookie_ = !0);\n  this.replaceLastScript(c);\n};\npagespeed."
    "LocalStorageCache.prototype.inlineCss = pagespeed.LocalStora"
    "geCache.prototype.inlineCss;\npagespeed.LocalStorageCache.pro"
    "totype.inlineImg = function(a, b) {\n  var c = window.localSt"
    "orage.getItem(\"pagespeed_lsc_url:\" + a + \" pagespeed_lsc_has"
    "h:\" + b), d = document.createElement(\"img\");\n  c && !this.ha"
    "sExpired(c) ? d.src = this.getData(c) : (d.src = a, this.reg"
    "enerate_cookie_ = !0);\n  c = 2;\n  for (var f = arguments.len"
    "gth; c < f; ++c) {\n    var e = arguments[c].indexOf(\"=\");\n  "
    "  d.setAttribute(arguments[c].substring(0, e), arguments[c]."
    "substring(e + 1));\n  }\n  this.replaceLastScript(d);\n};\npages"
    "peed.LocalStorageCache.prototype.inlineImg = pagespeed.Local"
    "StorageCache.prototype.inlineImg;\npagespeed.LocalStorageCach"
    "e.prototype.processTags_ = function(a, b, c) {\n  a = documen"
    "t.getElementsByTagName(a);\n  for (var d = 0, f = a.length; d"
    " < f; ++d) {\n    var e = a[d], h = e.getAttribute(\"data-page"
    "speed-lsc-hash\"), g = e.getAttribute(\"data-pagespeed-lsc-url"
    "\");\n    if (h && g) {\n      g = \"pagespeed_lsc_url:\" + g;\n  "
    "    b && (g += \" pagespeed_lsc_hash:\" + h);\n      var k = e."
    "getAttribute(\"data-pagespeed-lsc-expiry\");\n      k = k ? (ne"
    "w Date(k)).getTime() : \"\";\n      e = c(e);\n      if (!e) {\n "
    "       var l = window.localStorage.getItem(g);\n        l && "
    "(e = this.getData(l));\n      }\n      e && (window.localStora"
    "ge.setItem(g, k + \" \" + h + \" \" + e), this.regenerate_cookie"
    "_ = !0);\n    }\n  }\n};\npagespeed.LocalStorageCache.prototype."
    "saveInlinedData_ = function() {\n  this.processTags_(\"img\", !"
    "0, function(a) {\n    return a.src;\n  });\n  this.processTags_"
    "(\"style\", !1, function(a) {\n    return a.firstChild ? a.firs"
    "tChild.nodeValue : null;\n  });\n};\npagespeed.LocalStorageCach"
    "e.prototype.generateCookie_ = function() {\n  if (this.regene"
    "rate_cookie_) {\n    for (var a = [], b = [], c = 0, d = page"
    "speedutils.now(), f = 0, e = window.localStorage.length; f <"
    " e; ++f) {\n      var h = window.localStorage.key(f);\n      i"
    "f (!h.indexOf(\"pagespeed_lsc_url:\")) {\n        var g = windo"
    "w.localStorage.getItem(h), k = g.indexOf(\" \"), l = parseInt("
    "g.substring(0, k), 10);\n        if (!isNaN(l)) {\n          i"
    "f (l <= d) {\n            a.push(h);\n            continue;\n  "
    "        } else if (l < c || c == 0) {\n            c = l;\n   "
    "       }\n        }\n        h = g.indexOf(\" \", k + 1);\n      "
    "  g = g.substring(k + 1, h);\n        b.push(g);\n      }\n    "
    "}\n    d = \"\";\n    c && (d = \"; expires=\" + (new Date(c)).toU"
    "TCString());\n    document.cookie = \"_GPSLSC=\" + b.join(\"!\") "
    "+ d;\n    f = 0;\n    for (e = a.length; f < e; ++f) {\n      w"
    "indow.localStorage.removeItem(a[f]);\n    }\n    this.regenera"
    "te_cookie_ = !1;\n  }\n};\npagespeed.localStorageCacheInit = fu"
    "nction() {\n  if (window.localStorage) {\n    var a = new page"
    "speed.LocalStorageCache();\n    pagespeed.localStorageCache ="
    " a;\n    pagespeedutils.addHandler(window, \"load\", function()"
    " {\n      a.saveInlinedData_();\n    });\n    pagespeedutils.ad"
    "dHandler(window, \"load\", function() {\n      a.generateCookie"
    "_();\n    });\n  }\n};\npagespeed.localStorageCacheInit = pagesp"
    "eed.localStorageCacheInit;\n})();\n";

}  // namespace net_instaweb
