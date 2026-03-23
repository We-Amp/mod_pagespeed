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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/critical_css_beacon_opt.js

namespace net_instaweb {

const char* JS_critical_css_beacon_opt =
    "(function(){function k(b){var a=window;if(a.addEventListener"
    ")a.addEventListener(\"load\",b,!1);else if(a.attachEvent)a.att"
    "achEvent(\"onload\",b);else{var d=a.onload;a.onload=function()"
    "{b.call(this);d&&d.call(this)}}};window.pagespeed=window.pag"
    "espeed||{};var n=window.pagespeed;function p(b,a,d,l,m){this"
    ".m=b;this.o=a;this.v=d;this.u=l;this.h=m;this.i=[];this.g=0}"
    "p.prototype.j=function(b){for(var a=0;a<250&&this.g<this.h.l"
    "ength;++a,++this.g)try{document.querySelector(this.h[this.g]"
    ")!=null&&this.i.push(this.h[this.g])}catch(d){}this.g<this.h"
    ".length?window.setTimeout(this.j.bind(this),0,b):b()};\nn.l=f"
    "unction(b,a,d,l,m){if(document.querySelector&&Function.proto"
    "type.bind){var e=new p(b,a,d,l,m);k(function(){window.setTim"
    "eout(function(){e.j(function(){var g=\"oh=\"+e.v+\"&n=\"+e.u;g+="
    "\"&cs=\";for(var c=0;c<e.i.length;++c){var h=c>0?\",\":\"\";h+=enc"
    "odeURIComponent(e.i[c]);if(g.length+h.length>131072)break;g+"
    "=h}n.criticalCssBeaconData=g;c=e.m;h=e.o;if(window.XMLHttpRe"
    "quest)var f=new XMLHttpRequest;else if(window.ActiveXObject)"
    "try{f=new ActiveXObject(\"Msxml2.XMLHTTP\")}catch(q){try{f=new"
    " ActiveXObject(\"Microsoft.XMLHTTP\")}catch(r){}}f&&\n(c=c+(c.i"
    "ndexOf(\"?\")==-1?\"?\":\"&\")+\"url=\"+encodeURIComponent(h),f.open"
    "(\"POST\",c),f.setRequestHeader(\"Content-Type\",\"application/x-"
    "www-form-urlencoded\"),f.send(g))})},0)})}};n.criticalCssBeac"
    "onInit=n.l;})();\n";

}  // namespace net_instaweb
