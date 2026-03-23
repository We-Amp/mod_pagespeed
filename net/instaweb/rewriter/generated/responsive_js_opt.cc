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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/responsive_js_opt.js

namespace net_instaweb {

const char* JS_responsive_js_opt =
    "(function(){function h(a,c,b){return a.call.apply(a.bind,arg"
    "uments)}function k(a,c,b){if(!a)throw Error();if(arguments.l"
    "ength>2){var e=Array.prototype.slice.call(arguments,2);retur"
    "n function(){var d=Array.prototype.slice.call(arguments);Arr"
    "ay.prototype.unshift.apply(d,e);return a.apply(c,d)}}return "
    "function(){return a.apply(c,arguments)}}function l(a,c,b){l="
    "Function.prototype.bind&&Function.prototype.bind.toString()."
    "indexOf(\"native code\")!=-1?h:k;return l.apply(null,arguments"
    ")};function m(){}m.prototype.toString=function(){return\"IMG\""
    "};var n=new m;function p(){this.g=\"\"}p.prototype.toString=fu"
    "nction(){return\"SafeScript{\"+this.g+\"}\"};p.prototype.h=funct"
    "ion(a){this.g=a};(new p).h(\"\");function q(){this.l=\"\"}q.prot"
    "otype.toString=function(){return\"SafeStyle{\"+this.l+\"}\"};q.p"
    "rototype.h=function(a){this.l=a};(new q).h(\"\");function r(){"
    "this.j=\"\"}r.prototype.toString=function(){return\"SafeStyleSh"
    "eet{\"+this.j+\"}\"};r.prototype.h=function(a){this.j=a};(new r"
    ").h(\"\");function t(){this.g=\"\"}t.prototype.toString=function"
    "(){return\"SafeHtml{\"+this.g+\"}\"};t.prototype.h=function(a){t"
    "his.g=a};(new t).h(\"<!DOCTYPE html>\");(new t).h(\"\");(new t)."
    "h(\"<br>\");function u(a){var c=Number(a);return c==0&&/^[\\s\\x"
    "a0]*$/.test(a)?NaN:c};function v(a){return window.matchMedia"
    "(\"(min-resolution: \"+a+\"dppx),(min--moz-device-pixel-ratio: "
    "\"+a+\"),(min-resolution: \"+a*96+\"dpi)\").matches?a:0};function"
    " w(a,c){this.resolution=a;this.url=c}function x(a){this.m=a;"
    "this.i=0;this.g=[]}function y(){this.i=[]}function z(a,c){va"
    "r b=new Image;b.onload=function(){a.src=c};b.src=c}\ny.protot"
    "ype.g=function(){var a=document.documentElement.clientWidth/"
    "window.innerWidth;var c=window;a=(c.devicePixelRatio!==void "
    "0?c.devicePixelRatio:c.matchMedia?v(3)||v(2)||v(1.5)||v(1)||"
    ".75:1)*a;c=this.i.length;for(var b=0;b<c;++b){var e=this.i[b"
    "],d=a;if(d>e.i)for(var g=e.g.length,f=0;f<g;++f)if(d<=e.g[f]"
    ".resolution){e.i=e.g[f].resolution;z(e.m,e.g[f].url);break}}"
    "};function A(a,c){c=a.search(c);return c==-1?a.length:c}var "
    "B=/[ \\t\\n\\f\\r]/,C=/[^ \\t\\n\\f\\r]/,D=/[ \\t\\n\\f\\r,]/,E=/[^ \\t\\n"
    "\\f\\r,]/;\nfunction F(a,c,b){a=new x(a);var e=!1,d=A(b,E);for("
    "b=b.slice(d);b.length>0;){d=A(b,B);var g=b.slice(0,d);b=b.sl"
    "ice(d);if(g[g.length-1]==\",\")return null;d=A(b,C);b=b.slice("
    "d);d=A(b,D);var f=b.slice(0,d);b=b.slice(d);if(f.length>1&&f"
    "[f.length-1]==\"x\"){d=u(f.slice(0,-1));if(isNaN(d))return nul"
    "l;a.g.push(new w(d,g));d==1&&(e=!0)}else return null;d=A(b,C"
    ");b=b.slice(d);if(b.length>0&&b[0]!=\",\")return null;b=b.slic"
    "e(1);d=A(b,E);b=b.slice(d)}!e&&c&&a.g.push(new w(1,c));a.g.s"
    "ort(function(G,H){return G.resolution-\nH.resolution});return"
    " a}y.prototype.init=function(){for(var a=document.getElement"
    "sByTagName(String(n)),c=0,b;b=a[c];++c){var e=b.getAttribute"
    "(\"src\"),d=b.getAttribute(\"srcset\");d&&(b=F(b,e,d),b!=null&&t"
    "his.i.push(b))}window.addEventListener(\"resize\",l(this.g,thi"
    "s));window.addEventListener(\"touchmove\",l(function(g){g.touc"
    "hes.length>1&&this.g()},this));this.g()};(new y).init();})()"
    ";\n";

}  // namespace net_instaweb
