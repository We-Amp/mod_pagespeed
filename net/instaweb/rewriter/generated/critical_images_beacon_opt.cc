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
    "(function(){var n=this||self;function p(a,d){a=a.split(\".\");"
    "var b=n;a[0]in b||typeof b.execScript==\"undefined\"||b.execSc"
    "ript(\"var \"+a[0]);for(var c;a.length&&(c=a.shift());)a.lengt"
    "h||d===void 0?b[c]&&b[c]!==Object.prototype[c]?b=b[c]:b=b[c]"
    "={}:b[c]=d};var r=class{constructor(a){if(q!==q)throw Error("
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
    "ring(){return this.g.toString()}}new y;function z(a){var d=w"
    "indow;if(d.addEventListener)d.addEventListener(\"load\",a,!1);"
    "else if(d.attachEvent)d.attachEvent(\"onload\",a);else{var b=d"
    ".onload;d.onload=function(){a.call(this);b&&b.call(this)}}};"
    "var A;function B(a,d,b,c,e){this.m=a;this.u=d;this.v=b;this."
    "j=e;this.l={height:window.innerHeight||document.documentElem"
    "ent.clientHeight||document.body.clientHeight,width:window.in"
    "nerWidth||document.documentElement.clientWidth||document.bod"
    "y.clientWidth};this.o=c;this.h={};this.g=[];this.i={}}functi"
    "on C(a){return a==\" \"||a==\"\\t\"||a==\"\\n\"||a==\"\\f\"||a==\"\\r\"}\nf"
    "unction D(a){var d=a.getAttribute(\"data-pagespeed-url-hash\")"
    ";if(d)return d;var b=a.getAttribute(\"data-pagespeed-srcset-u"
    "rl-hashes\");if(!b)return null;d=a.currentSrc;if(!d)return nu"
    "ll;var c=a.getAttribute(\"srcset\")||\"\";a=[];for(var e=0,h=c.l"
    "ength;;){for(;e<h&&(C(c.charAt(e))||c.charAt(e)==\",\");)++e;i"
    "f(e>=h)break;for(var f=e;f<h&&!C(c.charAt(f));)++f;var g=c.s"
    "ubstring(e,f);e=f;for(f=!0;g.charAt(g.length-1)==\",\";)g=g.su"
    "bstring(0,g.length-1),f=!1;if(f)for(f=!1;e<h;){var k=c.charA"
    "t(e);if(k==\"(\")f=!0;\nelse if(k==\")\"&&f)f=!1;else if(k==\",\"&&"
    "!f)break;++e}a.push(g)}b=b.split(\",\");for(c=0;c<a.length&&c<"
    "b.length;++c){if(e=b[c])e=document.createElement(\"a\"),e.href"
    "=a[c],e=e.href==d;if(e)return b[c]}return null}\nfunction E(a"
    ",d){var b,c=D(d);if(b=c&&!(c in a.i))if(d.offsetWidth<=0&&d."
    "offsetHeight<=0)b=!1;else{b=d.getBoundingClientRect();var e="
    "document.body;d=b.top+(\"pageYOffset\"in window?window.pageYOf"
    "fset:(document.documentElement||e.parentNode||e).scrollTop);"
    "b=b.left+(\"pageXOffset\"in window?window.pageXOffset:(documen"
    "t.documentElement||e.parentNode||e).scrollLeft);e=d.toString"
    "()+\",\"+b;a.h.hasOwnProperty(e)?b=!1:(a.h[e]=!0,b=d<=a.l.heig"
    "ht&&b<=a.l.width)}b&&(a.g.push(c),a.i[c]=!0)}\np(\"pagespeed.C"
    "riticalImages.checkImageForCriticality\",function(a){var d=A;"
    "a.getBoundingClientRect&&E(d,a)});p(\"pagespeed.CriticalImage"
    "s.checkCriticalImages\",function(){F(A)});\nfunction F(a){a.h="
    "{};for(var d=[\"IMG\",\"INPUT\"],b=[],c=0;c<d.length;++c){var e="
    "b.concat;var h=document.getElementsByTagName(d[c]);var f=h.l"
    "ength;if(f>0){var g=Array(f);for(var k=0;k<f;k++)g[k]=h[k];h"
    "=g}else h=[];b=e.call(b,h)}if(b.length!=0&&b[0].getBoundingC"
    "lientRect){for(c=0;d=b[c];++c)E(a,d);d=\"oh=\"+a.v;a.j&&(d+=\"&"
    "n=\"+a.j);if(b=a.g.length!=0)for(d+=\"&ci=\"+encodeURIComponent"
    "(a.g[0]),c=1;c<a.g.length;++c)e=\",\"+encodeURIComponent(a.g[c"
    "]),d.length+e.length<=131072&&(d+=e);if(a.o){c=encodeURIComp"
    "onent;\nb=JSON;e=b.stringify;h={};f=document.getElementsByTag"
    "Name(\"IMG\");if(f.length==0)h={};else if(g=f[0],\"naturalWidth"
    "\"in g&&\"naturalHeight\"in g)for(k=0;g=f[k];++k){var l=D(g);l&"
    "&(!(l in h)&&g.width>0&&g.height>0&&g.naturalWidth>0&&g.natu"
    "ralHeight>0||l in h&&g.width>=h[l].B&&g.height>=h[l].A)&&(h["
    "l]={rw:g.width,rh:g.height,ow:g.naturalWidth,oh:g.naturalHei"
    "ght})}else h={};e=\"&rd=\"+c(e.call(b,h));d.length+e.length<=1"
    "31072&&(d+=e);b=!0}G=d;if(b){c=a.m;a=a.u;if(window.XMLHttpRe"
    "quest)var m=new XMLHttpRequest;\nelse if(window.ActiveXObject"
    ")try{m=new ActiveXObject(\"Msxml2.XMLHTTP\")}catch(H){try{m=ne"
    "w ActiveXObject(\"Microsoft.XMLHTTP\")}catch(I){}}m&&(a=c+(c.i"
    "ndexOf(\"?\")==-1?\"?\":\"&\")+\"url=\"+encodeURIComponent(a),m.open"
    "(\"POST\",a),m.setRequestHeader(\"Content-Type\",\"application/x-"
    "www-form-urlencoded\"),m.send(d))}}}var G=\"\";p(\"pagespeed.Cri"
    "ticalImages.getBeaconData\",function(){return G});\np(\"pagespe"
    "ed.CriticalImages.Run\",function(a,d,b,c,e,h){var f=new B(a,d"
    ",b,e,h);A=f;c&&z(function(){window.setTimeout(function(){F(f"
    ")},0)})});})();\n";

}  // namespace net_instaweb
