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
// Automatically generated from critical_css_loader_opt.js

namespace net_instaweb {

const char* JS_critical_css_loader_opt =
    "(function(){function b(a,c){var f=d;if(a.addEventListener)a."
    "addEventListener(c,f,!1);else if(a.attachEvent)a.attachEvent"
    "(\"on\"+c,f);else{var k=a[\"on\"+c];a[\"on\"+c]=function(){f.call("
    "this);k&&k.call(this)}}};var e=!1;function g(a){a.getAttribu"
    "te(\"rel\")==\"preload\"&&a.setAttribute(\"rel\",\"stylesheet\")}fun"
    "ction h(a){(a=a.target)&&a.nodeName==\"LINK\"&&a.hasAttribute("
    "\"data-pagespeed-deferred-css\")&&g(a)}function d(){for(var a="
    "document.querySelectorAll(\"link[data-pagespeed-deferred-css]"
    "\"),c=0;c<a.length;++c)g(a[c])}\nfunction l(){if(!e){e=!0;docu"
    "ment.addEventListener(\"load\",h,!0);document.addEventListener"
    "(\"error\",h,!0);var a=document.createElement(\"link\");a.relLis"
    "t&&a.relList.supports&&a.relList.supports(\"preload\")||b(docu"
    "ment,\"DOMContentLoaded\");b(window,\"load\");d()}}var m=[\"pages"
    "peed\",\"CriticalCssLoader\",\"Run\"],n=this||self;m[0]in n||type"
    "of n.execScript==\"undefined\"||n.execScript(\"var \"+m[0]);for("
    "var p;m.length&&(p=m.shift());)m.length||l===void 0?n[p]&&n["
    "p]!==Object.prototype[p]?n=n[p]:n=n[p]={}:n[p]=l;})();\n";

}  // namespace net_instaweb
