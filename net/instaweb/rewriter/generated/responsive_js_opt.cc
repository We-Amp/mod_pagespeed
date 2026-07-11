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
// Automatically generated from responsive_js_opt.js

namespace net_instaweb {

const char* JS_responsive_js_opt =
    "(function(){var h=this||self;function k(b,c,a){return b.call"
    ".apply(b.bind,arguments)}function l(b,c,a){if(!b)throw Error"
    "();if(arguments.length>2){var e=Array.prototype.slice.call(a"
    "rguments,2);return function(){var d=Array.prototype.slice.ca"
    "ll(arguments);Array.prototype.unshift.apply(d,e);return b.ap"
    "ply(c,d)}}return function(){return b.apply(c,arguments)}}fun"
    "ction m(b,c,a){m=Function.prototype.bind&&Function.prototype"
    ".bind.toString().indexOf(\"native code\")!=-1?k:l;return m.app"
    "ly(null,arguments)};var p=class{constructor(b){if(n!==n)thro"
    "w Error(\"SafeUrl is not meant to be built directly\");this.g="
    "b}toString(){return this.g.toString()}},n={};new p(\"about:in"
    "valid#zClosurez\");new p(\"about:blank\");const q={};class r{co"
    "nstructor(){if(q!==q)throw Error(\"SafeStyle is not meant to "
    "be built directly\");}toString(){return\"\".toString()}}new r;c"
    "onst t={};class u{constructor(){if(t!==t)throw Error(\"SafeSt"
    "yleSheet is not meant to be built directly\");}toString(){ret"
    "urn\"\".toString()}}new u;const v={};class w{constructor(){var"
    " b=h.trustedTypes&&h.trustedTypes.emptyHTML||\"\";if(v!==v)thr"
    "ow Error(\"SafeHtml is not meant to be built directly\");this."
    "g=b}toString(){return this.g.toString()}}new w;function x(b)"
    "{const c=Number(b);return c==0&&/^[\\s\\xa0]*$/.test(b)?NaN:c}"
    ";function y(b){return window.matchMedia(\"(min-resolution: \"+"
    "b+\"dppx),(min--moz-device-pixel-ratio: \"+b+\"),(min-resolutio"
    "n: \"+b*96+\"dpi)\").matches?b:0};function z(b,c){this.resoluti"
    "on=b;this.url=c}function A(b){this.i=b;this.h=0;this.g=[]}fu"
    "nction B(){this.h=[]}function C(b,c){var a=new Image;a.onloa"
    "d=function(){b.src=c};a.src=c}\nB.prototype.g=function(){var "
    "b=document.documentElement.clientWidth/window.innerWidth;var"
    " c=window;b=(c.devicePixelRatio!==void 0?c.devicePixelRatio:"
    "c.matchMedia?y(3)||y(2)||y(1.5)||y(1)||.75:1)*b;c=this.h.len"
    "gth;for(var a=0;a<c;++a){var e=this.h[a],d=b;if(d>e.h)for(va"
    "r g=e.g.length,f=0;f<g;++f)if(d<=e.g[f].resolution){e.h=e.g["
    "f].resolution;C(e.i,e.g[f].url);break}}};function D(b,c){c=b"
    ".search(c);return c==-1?b.length:c}var E=/[ \\t\\n\\f\\r]/,F=/[^"
    " \\t\\n\\f\\r]/,G=/[ \\t\\n\\f\\r,]/,H=/[^ \\t\\n\\f\\r,]/;\nfunction I(b"
    ",c,a){b=new A(b);var e=!1,d=D(a,H);for(a=a.slice(d);a.length"
    ">0;){d=D(a,E);var g=a.slice(0,d);a=a.slice(d);if(g[g.length-"
    "1]==\",\")return null;d=D(a,F);a=a.slice(d);d=D(a,G);var f=a.s"
    "lice(0,d);a=a.slice(d);if(f.length>1&&f[f.length-1]==\"x\"){d="
    "x(f.slice(0,-1));if(isNaN(d))return null;b.g.push(new z(d,g)"
    ");d==1&&(e=!0)}else return null;d=D(a,F);a=a.slice(d);if(a.l"
    "ength>0&&a[0]!=\",\")return null;a=a.slice(1);d=D(a,H);a=a.sli"
    "ce(d)}!e&&c&&b.g.push(new z(1,c));b.g.sort(function(J,K){ret"
    "urn J.resolution-\nK.resolution});return b}B.prototype.init=f"
    "unction(){for(var b=document.getElementsByTagName(\"IMG\"),c=0"
    ",a;a=b[c];++c){var e=a.getAttribute(\"src\"),d=a.getAttribute("
    "\"srcset\");d&&(a=I(a,e,d),a!=null&&this.h.push(a))}window.add"
    "EventListener(\"resize\",m(this.g,this));window.addEventListen"
    "er(\"touchmove\",m(function(g){g.touches.length>1&&this.g()},t"
    "his));this.g()};(new B).init();})();\n";

}  // namespace net_instaweb
