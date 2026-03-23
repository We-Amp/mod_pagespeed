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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/defer_iframe_opt.js

namespace net_instaweb {

const char* JS_defer_iframe_opt =
    "(function(){window.pagespeed=window.pagespeed||{};var b=wind"
    "ow.pagespeed;function c(){}c.prototype.h=function(){var a=do"
    "cument.getElementsByTagName(\"pagespeed_iframe\");if(a.length>"
    "0){a=a[0];for(var f=document.createElement(\"iframe\"),d=0,e=a"
    ".attributes,g=e.length;d<g;++d)f.setAttribute(e[d].name,e[d]"
    ".value);a.parentNode.replaceChild(f,a)}};c.prototype.convert"
    "ToIframe=c.prototype.h;b.g=function(){b.deferIframe=new c};b"
    ".deferIframeInit=b.g;})();\n";

}  // namespace net_instaweb
