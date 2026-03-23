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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/lazyload_images_opt.js

namespace net_instaweb {

const char* JS_lazyload_images_opt =
    "(function(){function f(a,b,d){if(a.addEventListener)a.addEve"
    "ntListener(b,d,!1);else if(a.attachEvent)a.attachEvent(\"on\"+"
    "b,d);else{var c=a[\"on\"+b];a[\"on\"+b]=function(){d.call(this);"
    "c&&c.call(this)}}};window.pagespeed=window.pagespeed||{};var"
    " g=window.pagespeed;function k(a){this.l=[];this.j=0;this.m="
    "!1;this.u=a;this.o=null;this.v=0;this.h=!1;this.g=0}function"
    " l(a,b){var d=b.getAttribute(\"data-pagespeed-lazy-position\")"
    ";if(d)return parseInt(d,0);d=b.offsetTop;var c=b.offsetParen"
    "t;c&&(d+=l(a,c));d=Math.max(d,0);b.setAttribute(\"data-pagesp"
    "eed-lazy-position\",d);return d}\nfunction m(a,b){if(!a.h&&(b."
    "offsetHeight==0||b.offsetWidth==0))return!1;a:if(b.currentSt"
    "yle)var d=b.currentStyle.position;else{if(document.defaultVi"
    "ew&&document.defaultView.getComputedStyle&&(d=document.defau"
    "ltView.getComputedStyle(b,null))){d=d.getPropertyValue(\"posi"
    "tion\");break a}d=b.style&&b.style.position?b.style.position:"
    "\"\"}if(d==\"relative\")return!0;var c=0;typeof window.pageYOffs"
    "et==\"number\"?c=window.pageYOffset:document.body&&document.bo"
    "dy.scrollTop?c=document.body.scrollTop:document.documentElem"
    "ent&&\ndocument.documentElement.scrollTop&&(c=document.docume"
    "ntElement.scrollTop);var e=window.innerHeight||document.docu"
    "mentElement.clientHeight||document.body.clientHeight;d=c;c+="
    "e;var h=b.getBoundingClientRect();h?(c=h.top-e,b=h.bottom):("
    "e=l(a,b),b=e+b.offsetHeight,c=e-c,b-=d);return c<=a.j&&b+a.j"
    ">=0}\nk.prototype.A=function(a){p(a);var b=this;window.setTim"
    "eout(function(){var d=a.getAttribute(\"data-pagespeed-lazy-sr"
    "c\");if(d)if((b.m||m(b,a))&&a.src.indexOf(b.u)!=-1){var c=a.p"
    "arentNode,e=a.nextSibling;c&&c.removeChild(a);a.i&&(a.getAtt"
    "ribute=a.i);a.removeAttribute(\"onload\");a.tagName&&a.tagName"
    "==\"IMG\"&&g.CriticalImages&&f(a,\"load\",function(){g.CriticalI"
    "mages.checkImageForCriticality(this);b.h&&(b.g--,b.g==0&&g.C"
    "riticalImages.checkCriticalImages())});a.removeAttribute(\"da"
    "ta-pagespeed-lazy-src\");a.removeAttribute(\"data-pagespeed-la"
    "zy-replaced-functions\");\nc&&c.insertBefore(a,e);if(c=a.getAt"
    "tribute(\"data-pagespeed-lazy-srcset\"))a.srcset=c,a.removeAtt"
    "ribute(\"data-pagespeed-lazy-srcset\");a.src=d}else b.l.push(a"
    ")},0)};k.prototype.loadIfVisibleAndMaybeBeacon=k.prototype.A"
    ";k.prototype.B=function(){this.m=!0;q(this)};k.prototype.loa"
    "dAllImages=k.prototype.B;function q(a){var b=a.l,d=b.length;"
    "a.l=[];for(var c=0;c<d;++c)a.A(b[c])}function t(a,b){return "
    "a.g?a.g(b)!=null:a.getAttribute(b)!=null}\nk.prototype.C=func"
    "tion(){for(var a=document.getElementsByTagName(\"img\"),b=0,d;"
    "d=a[b];b++)t(d,\"data-pagespeed-lazy-src\")&&p(d)};k.prototype"
    ".overrideAttributeFunctions=k.prototype.C;function p(a){t(a,"
    "\"data-pagespeed-lazy-replaced-functions\")||(a.i=a.getAttribu"
    "te,a.getAttribute=function(b){b.toLowerCase()==\"src\"&&t(this"
    ",\"data-pagespeed-lazy-src\")&&(b=\"data-pagespeed-lazy-src\");r"
    "eturn this.i(b)},a.setAttribute(\"data-pagespeed-lazy-replace"
    "d-functions\",\"1\"))}\ng.D=function(a,b){function d(){if(!(c.h&"
    "&a||c.o)){var e=200;(new Date).getTime()-c.v>200&&(e=0);c.o="
    "window.setTimeout(function(){c.v=(new Date).getTime();q(c);c"
    ".o=null},e)}}var c=new k(b);g.lazyLoadImages=c;f(window,\"loa"
    "d\",function(){c.h=!0;c.m=a;c.j=200;if(g.CriticalImages){for("
    "var e=0,h=document.getElementsByTagName(\"img\"),r=0,n;n=h[r];"
    "r++)n.src.indexOf(c.u)!=-1&&t(n,\"data-pagespeed-lazy-src\")&&"
    "e++;c.g=e;c.g==0&&g.CriticalImages.checkCriticalImages()}q(c"
    ")});b.indexOf(\"data\")!=0&&((new Image).src=\nb);f(window,\"scr"
    "oll\",d);f(window,\"resize\",d)};g.lazyLoadInit=g.D;})();\n";

}  // namespace net_instaweb
