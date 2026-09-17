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
// Automatically generated from lazyload_images_opt.js

namespace net_instaweb {

const char* JS_lazyload_images_opt =
    "(function(){function f(a,b,c){if(a.addEventListener)a.addEve"
    "ntListener(b,c,!1);else if(a.attachEvent)a.attachEvent(\"on\"+"
    "b,c);else{var d=a[\"on\"+b];a[\"on\"+b]=function(){c.call(this);"
    "d&&d.call(this)}}};window.pagespeed=window.pagespeed||{};var"
    " g=window.pagespeed;function k(a){this.l=[];this.j=0;this.m="
    "!1;this.v=a;this.u=null;this.A=0;this.h=!1;this.g=0}function"
    " m(a,b){var c=b.getAttribute(\"data-pagespeed-lazy-position\")"
    ";if(c)return parseInt(c,0);c=b.offsetTop;var d=b.offsetParen"
    "t;d&&(c+=m(a,d));c=Math.max(c,0);b.setAttribute(\"data-pagesp"
    "eed-lazy-position\",c);return c}\nfunction n(a,b){if(!a.h&&(b."
    "offsetHeight==0||b.offsetWidth==0))return!1;var c=0;typeof w"
    "indow.pageYOffset==\"number\"?c=window.pageYOffset:document.bo"
    "dy&&document.body.scrollTop?c=document.body.scrollTop:docume"
    "nt.documentElement&&document.documentElement.scrollTop&&(c=d"
    "ocument.documentElement.scrollTop);var d=window.innerHeight|"
    "|document.documentElement.clientHeight||document.body.client"
    "Height;var e=c;c+=d;var h=b.getBoundingClientRect();h?(c=h.t"
    "op-d,e=h.bottom):(d=m(a,b),b=d+b.offsetHeight,c=d-c,e=\nb-e);"
    "return c<=a.j&&e+a.j>=0}\nk.prototype.o=function(a){q(a);var "
    "b=this;window.setTimeout(function(){var c=a.getAttribute(\"da"
    "ta-pagespeed-lazy-src\");if(c)if((b.m||n(b,a))&&a.src.indexOf"
    "(b.v)!=-1){var d=a.parentNode,e=a.nextSibling;d&&d.removeChi"
    "ld(a);a.i&&(a.getAttribute=a.i);a.removeAttribute(\"onload\");"
    "if(a.tagName&&a.tagName==\"IMG\"&&g.CriticalImages){var h=!1,l"
    "=function(){h||(h=!0,b.h&&(b.g--,b.g==0&&g.CriticalImages.ch"
    "eckCriticalImages()))};f(a,\"load\",function(){g.CriticalImage"
    "s.checkImageForCriticality(this);l()});f(a,\"error\",\nfunction"
    "(){l()})}a.removeAttribute(\"data-pagespeed-lazy-src\");a.remo"
    "veAttribute(\"data-pagespeed-lazy-replaced-functions\");d&&d.i"
    "nsertBefore(a,e);if(d=a.getAttribute(\"data-pagespeed-lazy-sr"
    "cset\"))a.srcset=d,a.removeAttribute(\"data-pagespeed-lazy-src"
    "set\");a.src=c}else b.l.push(a)},0)};k.prototype.loadIfVisibl"
    "eAndMaybeBeacon=k.prototype.o;k.prototype.B=function(){this."
    "m=!0;r(this);for(var a=document.getElementsByTagName(\"img\"),"
    "b=0,c;c=a[b];b++)t(c,\"data-pagespeed-lazy-src\")&&this.o(c)};"
    "\nk.prototype.loadAllImages=k.prototype.B;function r(a){var b"
    "=a.l,c=b.length;a.l=[];for(var d=0;d<c;++d)a.o(b[d])}functio"
    "n t(a,b){return a.g?a.g(b)!=null:a.getAttribute(b)!=null}k.p"
    "rototype.C=function(){for(var a=document.getElementsByTagNam"
    "e(\"img\"),b=0,c;c=a[b];b++)t(c,\"data-pagespeed-lazy-src\")&&q("
    "c)};k.prototype.overrideAttributeFunctions=k.prototype.C;\nfu"
    "nction q(a){t(a,\"data-pagespeed-lazy-replaced-functions\")||("
    "a.i=a.getAttribute,a.getAttribute=function(b){b.toLowerCase("
    ")==\"src\"&&t(this,\"data-pagespeed-lazy-src\")&&(b=\"data-pagesp"
    "eed-lazy-src\");return this.i(b)},a.setAttribute(\"data-pagesp"
    "eed-lazy-replaced-functions\",\"1\"))}\ng.D=function(a,b){functi"
    "on c(){if(!(d.h&&a||d.u)){var e=200;(new Date).getTime()-d.A"
    ">200&&(e=0);d.u=window.setTimeout(function(){d.A=(new Date)."
    "getTime();r(d);d.u=null},e)}}var d=new k(b);g.lazyLoadImages"
    "=d;f(window,\"load\",function(){d.h=!0;d.m=a;d.j=200;if(g.Crit"
    "icalImages){for(var e=0,h=document.getElementsByTagName(\"img"
    "\"),l=0,p;p=h[l];l++)p.src.indexOf(d.v)!=-1&&t(p,\"data-pagesp"
    "eed-lazy-src\")&&e++;d.g=e;d.g==0&&g.CriticalImages.checkCrit"
    "icalImages()}r(d)});b.indexOf(\"data:\")!=0&&((new Image).src="
    "\nb);f(window,\"scroll\",c);f(window,\"resize\",c)};g.lazyLoadIni"
    "t=g.D;})();\n";

}  // namespace net_instaweb
