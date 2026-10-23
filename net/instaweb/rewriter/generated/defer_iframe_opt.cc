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
// Automatically generated from defer_iframe_opt.js

namespace net_instaweb {

const char* JS_defer_iframe_opt =
    "(()=>{window.pagespeed=window.pagespeed||{};var e=window.pag"
    "espeed;e.DeferIframe=function(){};e.DeferIframe.prototype.co"
    "nvertToIframe=function(){var r=document.getElementsByTagName"
    "(\"pagespeed_iframe\");if(r.length>0){for(var f=r[0],n=documen"
    "t.createElement(\"iframe\"),a=0,t=f.attributes,d=t.length;a<d;"
    "++a)n.setAttribute(t[a].name,t[a].value);f.parentNode.replac"
    "eChild(n,f)}};e.deferIframeInit=function(){var r=new e.Defer"
    "Iframe;e.deferIframe=r};})();\n";

}  // namespace net_instaweb
