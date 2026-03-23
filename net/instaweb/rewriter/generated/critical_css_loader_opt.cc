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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/critical_css_loader_opt.js

namespace net_instaweb {

const char* JS_critical_css_loader_opt =
    "(function(){function b(){var a=window,e=d;if(a.addEventListe"
    "ner)a.addEventListener(\"load\",e,!1);else if(a.attachEvent)a."
    "attachEvent(\"onload\",e);else{var c=a.onload;a.onload=functio"
    "n(){e.call(this);c&&c.call(this)}}};var f=!1;function d(){if"
    "(!f){f=!0;for(var a=document.getElementsByClassName(\"psa_add"
    "_styles\"),e=0,c;c=a[e];++e)if(c.nodeName==\"NOSCRIPT\"){var g="
    "document.createElement(\"div\");g.innerHTML=c.textContent;c=g."
    "childNodes;for(var h=0;h<c.length;++h)c[h].removeAttribute(\""
    "id\");document.body.appendChild(g)}}}\nfunction k(){var a=wind"
    "ow.requestAnimationFrame||window.webkitRequestAnimationFrame"
    "||window.mozRequestAnimationFrame||window.oRequestAnimationF"
    "rame||window.msRequestAnimationFrame||null;a?a(function(){wi"
    "ndow.setTimeout(d,0)}):b()}var l=[\"pagespeed\",\"CriticalCssLo"
    "ader\",\"Run\"],m=this||self;l[0]in m||typeof m.execScript==\"un"
    "defined\"||m.execScript(\"var \"+l[0]);for(var n;l.length&&(n=l"
    ".shift());)l.length||k===void 0?m[n]&&m[n]!==Object.prototyp"
    "e[n]?m=m[n]:m=m[n]={}:m[n]=k;})();\n";

}  // namespace net_instaweb
