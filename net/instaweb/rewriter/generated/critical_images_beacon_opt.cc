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
// Automatically generated from critical_images_beacon_opt.js

namespace net_instaweb {

const char* JS_critical_images_beacon_opt =
    "(function(){var n=this||self;function p(a,c){a=a.split(\".\");"
    "var b=n;a[0]in b||typeof b.execScript==\"undefined\"||b.execSc"
    "ript(\"var \"+a[0]);for(var d;a.length&&(d=a.shift());)a.lengt"
    "h||c===void 0?b[d]&&b[d]!==Object.prototype[d]?b=b[d]:b=b[d]"
    "={}:b[d]=c};var r=class{constructor(a){if(q!==q)throw Error("
    "\"SafeUrl is not meant to be built directly\");this.g=a}toStri"
    "ng(){return this.g.toString()}},q={};new r(\"about:invalid#zC"
    "losurez\");new r(\"about:blank\");const t={};class u{constructo"
    "r(){if(t!==t)throw Error(\"SafeStyle is not meant to be built"
    " directly\");}toString(){return\"\".toString()}}new u;const v={"
    "};class w{constructor(){if(v!==v)throw Error(\"SafeStyleSheet"
    " is not meant to be built directly\");}toString(){return\"\".to"
    "String()}}new w;const x={};class y{constructor(){var a=n.tru"
    "stedTypes&&n.trustedTypes.emptyHTML||\"\";if(x!==x)throw Error"
    "(\"SafeHtml is not meant to be built directly\");this.g=a}toSt"
    "ring(){return this.g.toString()}}new y;function z(a){var c=w"
    "indow;if(c.addEventListener)c.addEventListener(\"load\",a,!1);"
    "else if(c.attachEvent)c.attachEvent(\"onload\",a);else{var b=c"
    ".onload;c.onload=function(){a.call(this);b&&b.call(this)}}};"
    "var A;function B(a,c,b,d,e){this.m=a;this.u=c;this.v=b;this."
    "j=e;this.l={height:window.innerHeight||document.documentElem"
    "ent.clientHeight||document.body.clientHeight,width:window.in"
    "nerWidth||document.documentElement.clientWidth||document.bod"
    "y.clientWidth};this.o=d;this.h={};this.g=[];this.i={}}\nfunct"
    "ion C(a,c){var b,d=c.getAttribute(\"data-pagespeed-url-hash\")"
    ";if(b=d&&!(d in a.i))if(c.offsetWidth<=0&&c.offsetHeight<=0)"
    "b=!1;else{b=c.getBoundingClientRect();var e=document.body;c="
    "b.top+(\"pageYOffset\"in window?window.pageYOffset:(document.d"
    "ocumentElement||e.parentNode||e).scrollTop);b=b.left+(\"pageX"
    "Offset\"in window?window.pageXOffset:(document.documentElemen"
    "t||e.parentNode||e).scrollLeft);e=c.toString()+\",\"+b;a.h.has"
    "OwnProperty(e)?b=!1:(a.h[e]=!0,b=c<=a.l.height&&b<=a.l.width"
    ")}b&&(a.g.push(d),\na.i[d]=!0)}p(\"pagespeed.CriticalImages.ch"
    "eckImageForCriticality\",function(a){var c=A;a.getBoundingCli"
    "entRect&&C(c,a)});p(\"pagespeed.CriticalImages.checkCriticalI"
    "mages\",function(){D(A)});\nfunction D(a){a.h={};for(var c=[\"I"
    "MG\",\"INPUT\"],b=[],d=0;d<c.length;++d){var e=b.concat;var g=d"
    "ocument.getElementsByTagName(c[d]);var h=g.length;if(h>0){va"
    "r f=Array(h);for(var k=0;k<h;k++)f[k]=g[k];g=f}else g=[];b=e"
    ".call(b,g)}if(b.length!=0&&b[0].getBoundingClientRect){for(d"
    "=0;c=b[d];++d)C(a,c);c=\"oh=\"+a.v;a.j&&(c+=\"&n=\"+a.j);if(b=a."
    "g.length!=0)for(c+=\"&ci=\"+encodeURIComponent(a.g[0]),d=1;d<a"
    ".g.length;++d)e=\",\"+encodeURIComponent(a.g[d]),c.length+e.le"
    "ngth<=131072&&(c+=e);if(a.o){d=encodeURIComponent;\nb=JSON;e="
    "b.stringify;g={};h=document.getElementsByTagName(\"IMG\");if(h"
    ".length==0)g={};else if(f=h[0],\"naturalWidth\"in f&&\"naturalH"
    "eight\"in f)for(k=0;f=h[k];++k){var l=f.getAttribute(\"data-pa"
    "gespeed-url-hash\");l&&(!(l in g)&&f.width>0&&f.height>0&&f.n"
    "aturalWidth>0&&f.naturalHeight>0||l in g&&f.width>=g[l].B&&f"
    ".height>=g[l].A)&&(g[l]={rw:f.width,rh:f.height,ow:f.natural"
    "Width,oh:f.naturalHeight})}else g={};e=\"&rd=\"+d(e.call(b,g))"
    ";c.length+e.length<=131072&&(c+=e);b=!0}E=c;if(b){d=a.m;a=a."
    "u;if(window.XMLHttpRequest)var m=\nnew XMLHttpRequest;else if"
    "(window.ActiveXObject)try{m=new ActiveXObject(\"Msxml2.XMLHTT"
    "P\")}catch(F){try{m=new ActiveXObject(\"Microsoft.XMLHTTP\")}ca"
    "tch(G){}}m&&(a=d+(d.indexOf(\"?\")==-1?\"?\":\"&\")+\"url=\"+encodeU"
    "RIComponent(a),m.open(\"POST\",a),m.setRequestHeader(\"Content-"
    "Type\",\"application/x-www-form-urlencoded\"),m.send(c))}}}var "
    "E=\"\";p(\"pagespeed.CriticalImages.getBeaconData\",function(){r"
    "eturn E});\np(\"pagespeed.CriticalImages.Run\",function(a,c,b,d"
    ",e,g){var h=new B(a,c,b,e,g);A=h;d&&z(function(){window.setT"
    "imeout(function(){D(h)},0)})});})();\n";

}  // namespace net_instaweb
