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
    " pagespeed.DelayImages = function() {\n    this.lazyLoadHighR"
    "esHandlersRegistered_ = false;\n    this.highResReplaced_ = f"
    "alse;\n  };\n  pagespeed.DelayImages.prototype.replaceElementS"
    "rc = function(elements) {\n    for (var i = 0; i < elements.l"
    "ength; ++i) {\n      var src = elements[i].getAttribute(\"data"
    "-pagespeed-high-res-src\");\n      if (src) {\n        elements"
    "[i].setAttribute(\"src\", src);\n      }\n    }\n  };\n  pagespeed"
    ".DelayImages.prototype.registerLazyLoadHighRes = function() "
    "{\n    if (this.lazyLoadHighResHandlersRegistered_) {\n      t"
    "his.highResReplaced_ = false;\n      return;\n    }\n    var el"
    "em = document.body;\n    var interval = 500;\n    var tapStart"
    ", tapEnd = 0;\n    var me = this;\n    this.highResReplaced = "
    "false;\n    if (\"ontouchstart\" in elem) {\n      addHandler(el"
    "em, \"touchstart\", function(e) {\n        tapStart = now();\n  "
    "    });\n      addHandler(elem, \"touchend\", function(e) {\n   "
    "     tapEnd = now();\n        if (e.changedTouches != null &&"
    " e.changedTouches.length == 2 || e.touches != null && e.touc"
    "hes.length == 2 || tapEnd - tapStart < interval) {\n         "
    " me.loadHighRes();\n        }\n      });\n    } else {\n      ad"
    "dHandler(window, \"click\", function(e) {\n        me.loadHighR"
    "es();\n      });\n    }\n    addHandler(window, \"load\", functio"
    "n(e) {\n      me.loadHighRes();\n    });\n    this.lazyLoadHigh"
    "ResHandlersRegistered_ = true;\n  };\n  pagespeed.DelayImages."
    "prototype.loadHighRes = function() {\n    if (!this.highResRe"
    "placed_) {\n      this.replaceWithHighRes();\n      this.highR"
    "esReplaced_ = true;\n    }\n  };\n  pagespeed.DelayImages.proto"
    "type.replaceWithHighRes = function() {\n    this.replaceEleme"
    "ntSrc(document.getElementsByTagName(\"img\"));\n    this.replac"
    "eElementSrc(document.getElementsByTagName(\"input\"));\n  };\n  "
    "pagespeed.delayImagesInit = function() {\n    var temp = new "
    "pagespeed.DelayImages();\n    pagespeed.delayImages = temp;\n "
    " };\n})();\n";

}  // namespace net_instaweb
