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
    "(() => {\n  window.pagespeed = window.pagespeed || {};\n  var "
    "pagespeed = window.pagespeed;\n  pagespeed.DedupInlinedImages"
    " = function() {\n  };\n  pagespeed.DedupInlinedImages.prototyp"
    "e.inlineImg = function(from_img_id, to_img_id, script_id) {\n"
    "    var srcNode = document.getElementById(from_img_id);\n    "
    "if (!srcNode) {\n      return;\n    }\n    var dstNode = docume"
    "nt.getElementById(to_img_id);\n    if (!dstNode) {\n      retu"
    "rn;\n    }\n    var scriptNode = document.getElementById(scrip"
    "t_id);\n    if (!scriptNode) {\n      return;\n    }\n    dstNod"
    "e.src = srcNode.getAttribute(\"src\");\n    scriptNode.parentNo"
    "de.removeChild(scriptNode);\n  };\n  pagespeed.dedupInlinedIma"
    "gesInit = function() {\n    var dedupInlinedImages = new page"
    "speed.DedupInlinedImages();\n    pagespeed.dedupInlinedImages"
    " = dedupInlinedImages;\n  };\n})();\n";

}  // namespace net_instaweb
