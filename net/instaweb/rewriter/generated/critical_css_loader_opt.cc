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
    "(function(){function c(){var a=window,e=d;if(a.addEventListe"
    "ner)a.addEventListener(\"load\",e,!1);else if(a.attachEvent)a."
    "attachEvent(\"onload\",e);else{var b=a.onload;a.onload=functio"
    "n(){e.call(this);b&&b.call(this)}}};var f=!1;function d(){if"
    "(!f){f=!0;for(var a=document.getElementsByClassName(\"psa_add"
    "_styles\"),e=0,b;b=a[e];++e)if(b.nodeName==\"NOSCRIPT\"){var h="
    "document.createElement(\"div\");h.innerHTML=b.textContent;b=h."
    "childNodes;for(var g=0;g<b.length;++g)b[g].nodeType===1&&b[g"
    "].removeAttribute(\"id\");document.body.appendChild(h)}}}\nfunc"
    "tion k(){var a=window.requestAnimationFrame||window.webkitRe"
    "questAnimationFrame||window.mozRequestAnimationFrame||window"
    ".oRequestAnimationFrame||window.msRequestAnimationFrame||nul"
    "l;c();a&&a(function(){window.setTimeout(d,0)})}var l=[\"pages"
    "peed\",\"CriticalCssLoader\",\"Run\"],m=this||self;l[0]in m||type"
    "of m.execScript==\"undefined\"||m.execScript(\"var \"+l[0]);for("
    "var n;l.length&&(n=l.shift());)l.length||k===void 0?m[n]&&m["
    "n]!==Object.prototype[n]?m=m[n]:m=m[n]={}:m[n]=k;})();\n";

}  // namespace net_instaweb
