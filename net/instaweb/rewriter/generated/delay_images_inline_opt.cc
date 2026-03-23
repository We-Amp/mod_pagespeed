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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/delay_images_inline_opt.js

namespace net_instaweb {

const char* JS_delay_images_inline_opt =
    "(function(){window.pagespeed=window.pagespeed||{};var a=wind"
    "ow.pagespeed;function d(){this.h={}}d.prototype.j=function(c"
    ",b){this.h[c]=b};d.prototype.addLowResImages=d.prototype.j;d"
    ".prototype.g=function(c){for(var b=0;b<c.length;++b){var e=c"
    "[b].getAttribute(\"data-pagespeed-high-res-src\"),f=c[b].getAt"
    "tribute(\"src\");e&&!f&&(e=this.h[e])&&c[b].setAttribute(\"src\""
    ",e)}};d.prototype.replaceElementSrc=d.prototype.g;d.prototyp"
    "e.l=function(){this.g(document.getElementsByTagName(\"img\"));"
    "this.g(document.getElementsByTagName(\"input\"))};\nd.prototype"
    ".replaceWithLowRes=d.prototype.l;a.i=function(){a.delayImage"
    "sInline=new d};a.delayImagesInlineInit=a.i;})();\n";

}  // namespace net_instaweb
