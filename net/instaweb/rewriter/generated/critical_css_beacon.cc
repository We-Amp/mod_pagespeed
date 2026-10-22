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
    "(() => {\n  var MAX_POST_SIZE = 131072;\n  var sendBeacon = fu"
    "nction(beaconUrl, htmlUrl, data) {\n    var httpRequest;\n    "
    "if (window.XMLHttpRequest) {\n      httpRequest = new XMLHttp"
    "Request();\n    } else if (window.ActiveXObject) {\n      try "
    "{\n        httpRequest = new ActiveXObject(\"Msxml2.XMLHTTP\");"
    "\n      } catch (e) {\n        try {\n          httpRequest = n"
    "ew ActiveXObject(\"Microsoft.XMLHTTP\");\n        } catch (e2) "
    "{\n        }\n      }\n    }\n    if (!httpRequest) {\n      retu"
    "rn false;\n    }\n    var query_param_char = beaconUrl.indexOf"
    "(\"?\") == -1 ? \"?\" : \"&\";\n    var url = beaconUrl + query_par"
    "am_char + \"url=\" + encodeURIComponent(htmlUrl);\n    httpRequ"
    "est.open(\"POST\", url);\n    httpRequest.setRequestHeader(\n   "
    "   \"Content-Type\",\n      \"application/x-www-form-urlencoded\""
    "\n    );\n    httpRequest.send(data);\n    return true;\n  };\n  "
    "var addHandler = function(elem, eventName, func) {\n    if (e"
    "lem.addEventListener) {\n      elem.addEventListener(eventNam"
    "e, func, false);\n    } else if (elem.attachEvent) {\n      el"
    "em.attachEvent(\"on\" + eventName, func);\n    } else {\n      v"
    "ar oldHandler = elem[\"on\" + eventName];\n      elem[\"on\" + ev"
    "entName] = function() {\n        func.call(this);\n        if "
    "(oldHandler) {\n          oldHandler.call(this);\n        }\n  "
    "    };\n    }\n  };\n  var now = Date.now || (function() {\n    "
    "return +new Date();\n  });\n\n  window.pagespeed = window.pages"
    "peed || {};\n  var pagespeed = window.pagespeed;\n  pagespeed."
    "CriticalCssBeacon = function(beaconUrl, htmlUrl, optionsHash"
    ", nonce, selectors, opt_aboveTheFoldOnly) {\n    this.MAXITER"
    "S_ = 250;\n    this.MAXMEASURES_ = 10;\n    this.beaconUrl_ = "
    "beaconUrl;\n    this.htmlUrl_ = htmlUrl;\n    this.optionsHash"
    "_ = optionsHash;\n    this.nonce_ = nonce;\n    this.selectors"
    "_ = selectors;\n    this.aboveTheFoldOnly_ = !!opt_aboveTheFo"
    "ldOnly;\n    this.criticalSelectors_ = [];\n    this.idx_ = 0;"
    "\n  };\n  pagespeed.CriticalCssBeacon.prototype.sendBeacon_ = "
    "function() {\n    var data = \"oh=\" + this.optionsHash_ + \"&n="
    "\" + this.nonce_;\n    data += \"&cs=\";\n    var numSelectors = "
    "this.criticalSelectors_.length;\n    var start = Math.floor(M"
    "ath.random() * numSelectors);\n    var maxDataLen = MAX_POST_"
    "SIZE - \"&of=1\".length;\n    var truncated = false;\n    for (v"
    "ar i = 0; i < numSelectors; ++i) {\n      var tmp = i > 0 ? \""
    ",\" : \"\";\n      tmp += encodeURIComponent(\n        this.criti"
    "calSelectors_[(start + i) % numSelectors]\n      );\n      if "
    "(data.length + tmp.length > maxDataLen) {\n        truncated "
    "= true;\n        break;\n      }\n      data += tmp;\n    }\n    "
    "if (truncated) {\n      data += \"&of=1\";\n    }\n    pagespeed."
    "criticalCssBeaconData = data;\n    sendBeacon(this.beaconUrl_"
    ", this.htmlUrl_, data);\n  };\n  pagespeed.CriticalCssBeacon.p"
    "rototype.isSelectorCritical_ = function(selector) {\n    var "
    "elements = document.querySelectorAll(selector);\n    if (elem"
    "ents.length == 0) {\n      return false;\n    }\n    if (!this."
    "aboveTheFoldOnly_) {\n      return true;\n    }\n    var viewpo"
    "rtHeight = window.innerHeight || document.documentElement.cl"
    "ientHeight;\n    if (!viewportHeight || !elements[0].getBound"
    "ingClientRect) {\n      return true;\n    }\n    var scrollY = "
    "window.pageYOffset || 0;\n    var numToMeasure = Math.min(ele"
    "ments.length, this.MAXMEASURES_);\n    for (var i = 0; i < nu"
    "mToMeasure; ++i) {\n      var rect = elements[i].getBoundingC"
    "lientRect();\n      if (rect.width == 0 && rect.height == 0) "
    "{\n        return true;\n      }\n      if (rect.top + scrollY "
    "< viewportHeight) {\n        return true;\n      }\n    }\n    r"
    "eturn elements.length > numToMeasure;\n  };\n  pagespeed.Criti"
    "calCssBeacon.prototype.checkCssSelectors_ = function(callbac"
    "k) {\n    for (var i = 0; i < this.MAXITERS_ && this.idx_ < t"
    "his.selectors_.length; ++i, ++this.idx_) {\n      try {\n     "
    "   if (this.isSelectorCritical_(this.selectors_[this.idx_]))"
    " {\n          this.criticalSelectors_.push(this.selectors_[th"
    "is.idx_]);\n        }\n      } catch (e) {\n        if (!this.a"
    "boveTheFoldOnly_) {\n          this.criticalSelectors_.push(t"
    "his.selectors_[this.idx_]);\n        }\n        continue;\n    "
    "  }\n    }\n    if (this.idx_ < this.selectors_.length) {\n    "
    "  window.setTimeout(this.checkCssSelectors_.bind(this), 0, c"
    "allback);\n    } else {\n      callback();\n    }\n  };\n  pagesp"
    "eed.criticalCssBeaconInit = function(beaconUrl, htmlUrl, opt"
    "ionsHash, nonce, selectors, opt_aboveTheFoldOnly) {\n    if ("
    "!document.querySelector || !document.querySelectorAll || !Fu"
    "nction.prototype.bind) {\n      return;\n    }\n    var temp = "
    "new pagespeed.CriticalCssBeacon(\n      beaconUrl,\n      html"
    "Url,\n      optionsHash,\n      nonce,\n      selectors,\n      "
    "opt_aboveTheFoldOnly\n    );\n    var beacon_onload = function"
    "() {\n      window.setTimeout(function() {\n        temp.check"
    "CssSelectors_(function() {\n          temp.sendBeacon_();\n   "
    "     });\n      }, 0);\n    };\n    addHandler(window, \"load\", "
    "beacon_onload);\n  };\n})();\n";

}  // namespace net_instaweb
