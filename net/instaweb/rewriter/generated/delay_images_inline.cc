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
    "(function(){window.pagespeed = window.pagespeed || {};\nvar p"
    "agespeed = window.pagespeed;\npagespeed.DelayImagesInline = f"
    "unction() {\n  this.inlineMap_ = {};\n};\npagespeed.DelayImages"
    "Inline.prototype.addLowResImages = function(a, b) {\n  this.i"
    "nlineMap_[a] = b;\n};\npagespeed.DelayImagesInline.prototype.a"
    "ddLowResImages = pagespeed.DelayImagesInline.prototype.addLo"
    "wResImages;\npagespeed.DelayImagesInline.prototype.replaceEle"
    "mentSrc = function(a) {\n  for (var b = 0; b < a.length; ++b)"
    " {\n    var c = a[b].getAttribute(\"data-pagespeed-high-res-sr"
    "c\"), d = a[b].getAttribute(\"src\");\n    c && !d && (c = this."
    "inlineMap_[c]) && a[b].setAttribute(\"src\", c);\n  }\n};\npagesp"
    "eed.DelayImagesInline.prototype.replaceElementSrc = pagespee"
    "d.DelayImagesInline.prototype.replaceElementSrc;\npagespeed.D"
    "elayImagesInline.prototype.replaceWithLowRes = function() {\n"
    "  this.replaceElementSrc(document.getElementsByTagName(\"img\""
    "));\n  this.replaceElementSrc(document.getElementsByTagName(\""
    "input\"));\n};\npagespeed.DelayImagesInline.prototype.replaceWi"
    "thLowRes = pagespeed.DelayImagesInline.prototype.replaceWith"
    "LowRes;\npagespeed.delayImagesInlineInit = function() {\n  var"
    " a = new pagespeed.DelayImagesInline();\n  pagespeed.delayIma"
    "gesInline = a;\n};\npagespeed.delayImagesInlineInit = pagespee"
    "d.delayImagesInlineInit;\n})();\n";

}  // namespace net_instaweb
