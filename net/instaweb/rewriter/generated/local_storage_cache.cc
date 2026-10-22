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
// Automatically generated from local_storage_cache_dbg.js

namespace net_instaweb {

const char* JS_local_storage_cache =
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
    " pagespeed.LocalStorageCache = function() {\n    this.regener"
    "ate_cookie_ = true;\n  };\n  pagespeed.LocalStorageCache.proto"
    "type.hasExpired = function(obj) {\n    var expiry = parseInt("
    "obj.substring(0, obj.indexOf(\" \")), 10);\n    return !isNaN(e"
    "xpiry) && expiry <= now();\n  };\n  pagespeed.LocalStorageCach"
    "e.prototype.getData = function(obj) {\n    var pos1 = obj.ind"
    "exOf(\" \");\n    var pos2 = obj.indexOf(\" \", pos1 + 1);\n    re"
    "turn obj.substring(pos2 + 1);\n  };\n  pagespeed.LocalStorageC"
    "ache.prototype.replaceLastScript = function(newElement) {\n  "
    "  var scripts = document.getElementsByTagName(\"script\");\n   "
    " var lastElement = scripts[scripts.length - 1];\n    lastElem"
    "ent.parentNode.replaceChild(newElement, lastElement);\n  };\n "
    " pagespeed.LocalStorageCache.prototype.inlineCss = function("
    "url) {\n    var obj = null;\n    try {\n      obj = window.loca"
    "lStorage.getItem(\"pagespeed_lsc_url:\" + url);\n    } catch (e"
    ") {\n    }\n    var newNode = document.createElement(obj ? \"st"
    "yle\" : \"link\");\n    if (obj && !this.hasExpired(obj)) {\n    "
    "  newNode.type = \"text/css\";\n      newNode.appendChild(docum"
    "ent.createTextNode(this.getData(obj)));\n    } else {\n      n"
    "ewNode.rel = \"stylesheet\";\n      newNode.href = url;\n      t"
    "his.regenerate_cookie_ = true;\n    }\n    this.replaceLastScr"
    "ipt(newNode);\n  };\n  pagespeed.LocalStorageCache.prototype.i"
    "nlineImg = function(url, hash) {\n    var obj = null;\n    try"
    " {\n      obj = window.localStorage.getItem(\"pagespeed_lsc_ur"
    "l:\" + url + \" pagespeed_lsc_hash:\" + hash);\n    } catch (e) "
    "{\n    }\n    var newNode = document.createElement(\"img\");\n   "
    " if (obj && !this.hasExpired(obj)) {\n      newNode.src = thi"
    "s.getData(obj);\n    } else {\n      newNode.src = url;\n      "
    "this.regenerate_cookie_ = true;\n    }\n    for (var i = 2, n "
    "= arguments.length; i < n; ++i) {\n      var pos = arguments["
    "i].indexOf(\"=\");\n      newNode.setAttribute(\n        argumen"
    "ts[i].substring(0, pos),\n        arguments[i].substring(pos "
    "+ 1)\n      );\n    }\n    this.replaceLastScript(newNode);\n  }"
    ";\n  pagespeed.LocalStorageCache.prototype.processTags_ = fun"
    "ction(tagName, isHashInKey, dataFunc) {\n    var elements = d"
    "ocument.getElementsByTagName(tagName);\n    for (var i = 0, n"
    " = elements.length; i < n; ++i) {\n      var element = elemen"
    "ts[i];\n      var hash = element.getAttribute(\"data-pagespeed"
    "-lsc-hash\");\n      var url = element.getAttribute(\"data-page"
    "speed-lsc-url\");\n      if (hash && url) {\n        var urlkey"
    " = \"pagespeed_lsc_url:\" + url;\n        if (isHashInKey) {\n  "
    "        urlkey += \" pagespeed_lsc_hash:\" + hash;\n        }\n "
    "       var expiry = element.getAttribute(\"data-pagespeed-lsc"
    "-expiry\");\n        var millis = expiry ? new Date(expiry).ge"
    "tTime() : \"\";\n        var data = dataFunc(element);\n        "
    "if (!data) {\n          var obj = null;\n          try {\n     "
    "       obj = window.localStorage.getItem(urlkey);\n          "
    "} catch (e) {\n          }\n          if (obj) {\n            d"
    "ata = this.getData(obj);\n          }\n        }\n        if (d"
    "ata) {\n          try {\n            window.localStorage.setIt"
    "em(\n              urlkey,\n              millis + \" \" + hash "
    "+ \" \" + data\n            );\n            this.regenerate_cook"
    "ie_ = true;\n          } catch (e) {\n          }\n        }\n  "
    "    }\n    }\n  };\n  pagespeed.LocalStorageCache.prototype.sav"
    "eInlinedData_ = function() {\n    this.processTags_(\n      \"i"
    "mg\",\n      true,\n      function(e) {\n        return e.src;\n "
    "     }\n    );\n    this.processTags_(\n      \"style\",\n      fa"
    "lse,\n      function(e) {\n        return e.firstChild ? e.fir"
    "stChild.nodeValue : null;\n      }\n    );\n  };\n  pagespeed.Lo"
    "calStorageCache.prototype.generateCookie_ = function() {\n   "
    " if (this.regenerate_cookie_) {\n      var deadUns = [];\n    "
    "  var goodUns = [];\n      var minExpiry = 0;\n      var curre"
    "ntTime = now();\n      var numItems = 0;\n      try {\n        "
    "numItems = window.localStorage.length;\n      } catch (e) {\n "
    "     }\n      for (var i = 0, n = numItems; i < n; ++i) {\n   "
    "     try {\n          var key = window.localStorage.key(i);\n "
    "         if (key.indexOf(\"pagespeed_lsc_url:\")) continue;\n  "
    "        var obj = window.localStorage.getItem(key);\n        "
    "  var pos1 = obj.indexOf(\" \");\n          var expiry = parseI"
    "nt(obj.substring(0, pos1), 10);\n          if (!isNaN(expiry)"
    ") {\n            if (expiry <= currentTime) {\n              d"
    "eadUns.push(key);\n              continue;\n            } else"
    " if (expiry < minExpiry || minExpiry == 0) {\n              m"
    "inExpiry = expiry;\n            }\n          }\n          var p"
    "os2 = obj.indexOf(\" \", pos1 + 1);\n          var hash = obj.s"
    "ubstring(pos1 + 1, pos2);\n          goodUns.push(hash);\n    "
    "    } catch (e) {\n          continue;\n        }\n      }\n    "
    "  var expires = \"\";\n      if (minExpiry) expires = \"; expire"
    "s=\" + new Date(minExpiry).toUTCString();\n      document.cook"
    "ie = \"_GPSLSC=\" + goodUns.join(\"!\") + \";path=/\" + expires;\n "
    "     for (var i = 0, n = deadUns.length; i < n; ++i) {\n     "
    "   try {\n          window.localStorage.removeItem(deadUns[i]"
    ");\n        } catch (e) {\n        }\n      }\n      this.regene"
    "rate_cookie_ = false;\n    }\n  };\n  pagespeed.localStorageCac"
    "heInit = function() {\n    var temp = new pagespeed.LocalStor"
    "ageCache();\n    pagespeed.localStorageCache = temp;\n    addH"
    "andler(\n      window,\n      \"load\",\n      function() {\n     "
    "   temp.saveInlinedData_();\n      }\n    );\n    addHandler(\n "
    "     window,\n      \"load\",\n      function() {\n        temp.g"
    "enerateCookie_();\n      }\n    );\n  };\n})();\n";

}  // namespace net_instaweb
