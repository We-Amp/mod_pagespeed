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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/critical_images_beacon_opt.js

namespace net_instaweb {

const char* JS_critical_images_beacon_opt =
    "(function(){var n=this||self;function p(a,c){a=a.split(\".\");"
    "var b=n;a[0]in b||typeof b.execScript==\"undefined\"||b.execSc"
    "ript(\"var \"+a[0]);for(var d;a.length&&(d=a.shift());)a.lengt"
    "h||c===void 0?b[d]&&b[d]!==Object.prototype[d]?b=b[d]:b=b[d]"
    "={}:b[d]=c};function q(a){this.g=a}q.prototype.toString=func"
    "tion(){return this.g};var r=new q(\"IMG\"),t=new q(\"INPUT\");fu"
    "nction u(){this.g=\"\"}u.prototype.toString=function(){return\""
    "SafeScript{\"+this.g+\"}\"};u.prototype.h=function(a){this.g=a}"
    ";(new u).h(\"\");function v(){this.u=\"\"}v.prototype.toString=f"
    "unction(){return\"SafeStyle{\"+this.u+\"}\"};v.prototype.h=funct"
    "ion(a){this.u=a};(new v).h(\"\");function w(){this.o=\"\"}w.prot"
    "otype.toString=function(){return\"SafeStyleSheet{\"+this.o+\"}\""
    "};w.prototype.h=function(a){this.o=a};(new w).h(\"\");function"
    " x(){this.g=\"\"}x.prototype.toString=function(){return\"SafeHt"
    "ml{\"+this.g+\"}\"};x.prototype.h=function(a){this.g=a};(new x)"
    ".h(\"<!DOCTYPE html>\");(new x).h(\"\");(new x).h(\"<br>\");functi"
    "on y(a){var c=window;if(c.addEventListener)c.addEventListene"
    "r(\"load\",a,!1);else if(c.attachEvent)c.attachEvent(\"onload\","
    "a);else{var b=c.onload;c.onload=function(){a.call(this);b&&b"
    ".call(this)}}};var z;function A(a,c,b,d,e){this.v=a;this.B=c"
    ";this.C=b;this.l=e;this.m={height:window.innerHeight||docume"
    "nt.documentElement.clientHeight||document.body.clientHeight,"
    "width:window.innerWidth||document.documentElement.clientWidt"
    "h||document.body.clientWidth};this.A=d;this.i={};this.g=[];t"
    "his.j={}}\nfunction B(a,c){var b,d=c.getAttribute(\"data-pages"
    "peed-url-hash\");if(b=d&&!(d in a.j))if(c.offsetWidth<=0&&c.o"
    "ffsetHeight<=0)b=!1;else{b=c.getBoundingClientRect();var e=d"
    "ocument.body;c=b.top+(\"pageYOffset\"in window?window.pageYOff"
    "set:(document.documentElement||e.parentNode||e).scrollTop);b"
    "=b.left+(\"pageXOffset\"in window?window.pageXOffset:(document"
    ".documentElement||e.parentNode||e).scrollLeft);e=c.toString("
    ")+\",\"+b;a.i.hasOwnProperty(e)?b=!1:(a.i[e]=!0,b=c<=a.m.heigh"
    "t&&b<=a.m.width)}b&&(a.g.push(d),\na.j[d]=!0)}p(\"pagespeed.Cr"
    "iticalImages.checkImageForCriticality\",function(a){var c=z;a"
    ".getBoundingClientRect&&B(c,a)});p(\"pagespeed.CriticalImages"
    ".checkCriticalImages\",function(){C(z)});\nfunction C(a){a.i={"
    "};for(var c=[r,t],b=[],d=0;d<c.length;++d){var e=b.concat;va"
    "r g=document.getElementsByTagName(c[d]);var h=g.length;if(h>"
    "0){for(var f=Array(h),k=0;k<h;k++)f[k]=g[k];g=f}else g=[];b="
    "e.call(b,g)}if(b.length!=0&&b[0].getBoundingClientRect){for("
    "d=0;c=b[d];++d)B(a,c);c=\"oh=\"+a.C;a.l&&(c+=\"&n=\"+a.l);if(b=a"
    ".g.length!=0)for(c+=\"&ci=\"+encodeURIComponent(a.g[0]),d=1;d<"
    "a.g.length;++d)e=\",\"+encodeURIComponent(a.g[d]),c.length+e.l"
    "ength<=131072&&(c+=e);if(a.A){d=encodeURIComponent;b=JSON;e="
    "b.stringify;\ng={};h=document.getElementsByTagName(String(r))"
    ";if(h.length==0)g={};else if(f=h[0],\"naturalWidth\"in f&&\"nat"
    "uralHeight\"in f)for(k=0;f=h[k];++k){var l=f.getAttribute(\"da"
    "ta-pagespeed-url-hash\");l&&(!(l in g)&&f.width>0&&f.height>0"
    "&&f.naturalWidth>0&&f.naturalHeight>0||l in g&&f.width>=g[l]"
    ".F&&f.height>=g[l].D)&&(g[l]={rw:f.width,rh:f.height,ow:f.na"
    "turalWidth,oh:f.naturalHeight})}else g={};e=\"&rd=\"+d(e.call("
    "b,g));c.length+e.length<=131072&&(c+=e);b=!0}D=c;if(b){d=a.v"
    ";a=a.B;if(window.XMLHttpRequest)var m=\nnew XMLHttpRequest;el"
    "se if(window.ActiveXObject)try{m=new ActiveXObject(\"Msxml2.X"
    "MLHTTP\")}catch(E){try{m=new ActiveXObject(\"Microsoft.XMLHTTP"
    "\")}catch(F){}}m&&(a=d+(d.indexOf(\"?\")==-1?\"?\":\"&\")+\"url=\"+en"
    "codeURIComponent(a),m.open(\"POST\",a),m.setRequestHeader(\"Con"
    "tent-Type\",\"application/x-www-form-urlencoded\"),m.send(c))}}"
    "}var D=\"\";p(\"pagespeed.CriticalImages.getBeaconData\",functio"
    "n(){return D});\np(\"pagespeed.CriticalImages.Run\",function(a,"
    "c,b,d,e,g){var h=new A(a,c,b,e,g);z=h;d&&y(function(){window"
    ".setTimeout(function(){C(h)},0)})});})();\n";

}  // namespace net_instaweb
