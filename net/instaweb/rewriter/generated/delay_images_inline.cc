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
// Automatically generated from delay_images_inline_dbg.js

namespace net_instaweb {

const char* JS_delay_images_inline =
    "(() => {\n  window.pagespeed = window.pagespeed || {};\n  var "
    "pagespeed = window.pagespeed;\n  pagespeed.DelayImagesInline "
    "= function() {\n    this.inlineMap_ = {};\n  };\n  pagespeed.De"
    "layImagesInline.prototype.addLowResImages = function(url, lo"
    "wResImage) {\n    this.inlineMap_[url] = lowResImage;\n  };\n  "
    "pagespeed.DelayImagesInline.prototype.replaceElementSrc = fu"
    "nction(elements) {\n    for (var i = 0; i < elements.length; "
    "++i) {\n      var high_res_src = elements[i].getAttribute(\"da"
    "ta-pagespeed-high-res-src\");\n      var src = elements[i].get"
    "Attribute(\"src\");\n      if (high_res_src && !src) {\n        "
    "var low_res = this.inlineMap_[high_res_src];\n        if (low"
    "_res) {\n          elements[i].setAttribute(\"src\", low_res);\n"
    "        }\n      }\n    }\n  };\n  pagespeed.DelayImagesInline.p"
    "rototype.replaceWithLowRes = function() {\n    this.replaceEl"
    "ementSrc(document.getElementsByTagName(\"img\"));\n    this.rep"
    "laceElementSrc(document.getElementsByTagName(\"input\"));\n  };"
    "\n  pagespeed.delayImagesInlineInit = function() {\n    var te"
    "mp = new pagespeed.DelayImagesInline();\n    pagespeed.delayI"
    "magesInline = temp;\n  };\n})();\n";

}  // namespace net_instaweb
