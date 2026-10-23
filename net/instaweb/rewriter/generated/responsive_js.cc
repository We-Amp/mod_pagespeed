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
// Automatically generated from responsive_js_dbg.js

namespace net_instaweb {

const char* JS_responsive_js =
    "(() => {\n  var toNumber = function(str) {\n    var num = Numb"
    "er(str);\n    if (num == 0 && /^[\\s\\xa0]*$/.test(str)) {\n    "
    "  return NaN;\n    }\n    return num;\n  };\n  var getPixelRatio"
    " = function() {\n    return window.devicePixelRatio !== void "
    "0 ? window.devicePixelRatio : 1;\n  };\n\n  window.pagespeed = "
    "window.pagespeed || {};\n  var pagespeed = window.pagespeed;\n"
    "  pagespeed.ResponsiveImageCandidate = function(resolution, "
    "url) {\n    this.resolution = resolution;\n    this.url = url;"
    "\n  };\n  pagespeed.ResponsiveImage = function(img) {\n    this"
    ".img = img;\n    this.currentResolution = 0;\n    this.availab"
    "leResolutions = [];\n  };\n  pagespeed.Responsive = function()"
    " {\n    this.allImages_ = [];\n  };\n  pagespeed.Responsive.upd"
    "ateImgSrc_ = function(img, url) {\n    var tempImg = new Imag"
    "e();\n    tempImg.onload = function() {\n      img.src = url;\n"
    "    };\n    tempImg.src = url;\n  };\n  pagespeed.ResponsiveIma"
    "ge.prototype.responsiveResize = function(devicePixelRatio) {"
    "\n    if (devicePixelRatio > this.currentResolution) {\n      "
    "var numResolutions = this.availableResolutions.length;\n     "
    " for (var i = 0; i < numResolutions; ++i) {\n        if (devi"
    "cePixelRatio <= this.availableResolutions[i].resolution) {\n "
    "         this.currentResolution = this.availableResolutions["
    "i].resolution;\n          pagespeed.Responsive.updateImgSrc_("
    "\n            this.img,\n            this.availableResolutions"
    "[i].url\n          );\n          break;\n        }\n      }\n    "
    "}\n  };\n  pagespeed.Responsive.prototype.computeDevicePixelRa"
    "tioWithZoom = function() {\n    var zoomRatio = document.docu"
    "mentElement.clientWidth / window.innerWidth;\n    return getP"
    "ixelRatio() * zoomRatio;\n  };\n  pagespeed.Responsive.prototy"
    "pe.responsiveResize = function() {\n    var devicePixelRatio "
    "= this.computeDevicePixelRatioWithZoom();\n    var numImages "
    "= this.allImages_.length;\n    for (var i = 0; i < numImages;"
    " ++i) {\n      this.allImages_[i].responsiveResize(devicePixe"
    "lRatio);\n    }\n  };\n  pagespeed.Responsive.search_ = functio"
    "n(str, re) {\n    var offset = str.search(re);\n    if (offset"
    " == -1) {\n      return str.length;\n    } else {\n      return"
    " offset;\n    }\n  };\n  var WHITESPACE = /[ \\t\\n\\f\\r]/;\n  var "
    "NOT_WHITESPACE = /[^ \\t\\n\\f\\r]/;\n  var WHITESPACE_OR_COMMA ="
    " /[ \\t\\n\\f\\r,]/;\n  var NOT_WHITESPACE_OR_COMMA = /[^ \\t\\n\\f\\"
    "r,]/;\n  pagespeed.Responsive.prototype.parseSrcset = functio"
    "n(img, src, srcset) {\n    var respImage = new pagespeed.Resp"
    "onsiveImage(img);\n    var has_1x = false;\n    var rest = src"
    "set;\n    var pos = pagespeed.Responsive.search_(rest, NOT_WH"
    "ITESPACE_OR_COMMA);\n    rest = rest.slice(pos);\n    while (r"
    "est.length > 0) {\n      pos = pagespeed.Responsive.search_(r"
    "est, WHITESPACE);\n      var url = rest.slice(0, pos);\n      "
    "rest = rest.slice(pos);\n      if (url[url.length - 1] == \",\""
    ") {\n        return null;\n      }\n      pos = pagespeed.Respo"
    "nsive.search_(rest, NOT_WHITESPACE);\n      rest = rest.slice"
    "(pos);\n      pos = pagespeed.Responsive.search_(rest, WHITES"
    "PACE_OR_COMMA);\n      var descriptor = rest.slice(0, pos);\n "
    "     rest = rest.slice(pos);\n      if (descriptor.length > 1"
    " && descriptor[descriptor.length - 1] == \"x\") {\n        var "
    "resolution = toNumber(descriptor.slice(0, -1));\n        if ("
    "isNaN(resolution)) {\n          return null;\n        }\n      "
    "  respImage.availableResolutions.push(\n          new pagespe"
    "ed.ResponsiveImageCandidate(resolution, url)\n        );\n    "
    "    if (resolution == 1) {\n          has_1x = true;\n        "
    "}\n      } else {\n        return null;\n      }\n      pos = pa"
    "gespeed.Responsive.search_(rest, NOT_WHITESPACE);\n      rest"
    " = rest.slice(pos);\n      if (rest.length > 0 && rest[0] != "
    "\",\") {\n        return null;\n      } else {\n        rest = re"
    "st.slice(1);\n      }\n      pos = pagespeed.Responsive.search"
    "_(rest, NOT_WHITESPACE_OR_COMMA);\n      rest = rest.slice(po"
    "s);\n    }\n    if (!has_1x && src) {\n      respImage.availabl"
    "eResolutions.push(\n        new pagespeed.ResponsiveImageCand"
    "idate(1, src)\n      );\n    }\n    respImage.availableResoluti"
    "ons.sort(function(a, b) {\n      return a.resolution - b.reso"
    "lution;\n    });\n    return respImage;\n  };\n  pagespeed.Respo"
    "nsive.prototype.init = function() {\n    var images = documen"
    "t.getElementsByTagName(\"IMG\");\n    for (var i = 0, img; img "
    "= images[i]; ++i) {\n      var src = img.getAttribute(\"src\");"
    "\n      var srcset = img.getAttribute(\"srcset\");\n      if (sr"
    "cset) {\n        var respImage = this.parseSrcset(img, src, s"
    "rcset);\n        if (respImage != null) {\n          this.allI"
    "mages_.push(respImage);\n        }\n      }\n    }\n    window.a"
    "ddEventListener(\"resize\", this.responsiveResize.bind(this));"
    "\n    window.addEventListener(\"touchmove\", (event) => {\n     "
    " if (event.touches.length > 1) {\n        this.responsiveResi"
    "ze();\n      }\n    });\n    this.responsiveResize();\n  };\n  pa"
    "gespeed.responsiveInstance = new pagespeed.Responsive();\n  p"
    "agespeed.responsiveInstance.init();\n})();\n";

}  // namespace net_instaweb
