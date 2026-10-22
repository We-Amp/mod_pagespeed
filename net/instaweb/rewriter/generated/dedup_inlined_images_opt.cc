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
// Automatically generated from dedup_inlined_images_opt.js

namespace net_instaweb {

const char* JS_dedup_inlined_images_opt =
    "(()=>{window.pagespeed=window.pagespeed||{};var e=window.pag"
    "espeed;e.DedupInlinedImages=function(){};e.DedupInlinedImage"
    "s.prototype.inlineImg=function(n,r,p){var t=document.getElem"
    "entById(n);if(t){var i=document.getElementById(r);if(i){var "
    "d=document.getElementById(p);d&&(i.src=t.getAttribute(\"src\")"
    ",d.parentNode.removeChild(d))}}};e.dedupInlinedImagesInit=fu"
    "nction(){var n=new e.DedupInlinedImages;e.dedupInlinedImages"
    "=n};})();\n";

}  // namespace net_instaweb
