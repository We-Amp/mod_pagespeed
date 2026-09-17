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
// Automatically generated from dedup_inlined_images_dbg.js

namespace net_instaweb {

const char* JS_dedup_inlined_images =
    "(function(){window.pagespeed = window.pagespeed || {};\nvar p"
    "agespeed = window.pagespeed;\npagespeed.DedupInlinedImages = "
    "function() {\n};\npagespeed.DedupInlinedImages.prototype.inlin"
    "eImg = function(a, c, b) {\n  if (a = document.getElementById"
    "(a)) {\n    if (c = document.getElementById(c)) {\n      if (b"
    " = document.getElementById(b)) {\n        c.src = a.getAttrib"
    "ute(\"src\"), b.parentNode.removeChild(b);\n      }\n    }\n  }\n}"
    ";\npagespeed.DedupInlinedImages.prototype.inlineImg = pagespe"
    "ed.DedupInlinedImages.prototype.inlineImg;\npagespeed.dedupIn"
    "linedImagesInit = function() {\n  var a = new pagespeed.Dedup"
    "InlinedImages();\n  pagespeed.dedupInlinedImages = a;\n};\npage"
    "speed.dedupInlinedImagesInit = pagespeed.dedupInlinedImagesI"
    "nit;\n})();\n";

}  // namespace net_instaweb
