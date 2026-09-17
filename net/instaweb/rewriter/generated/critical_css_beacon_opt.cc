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
// Automatically generated from critical_css_beacon_opt.js

namespace net_instaweb {

const char* JS_critical_css_beacon_opt =
    "(function(){function l(a){var b=window;if(b.addEventListener"
    ")b.addEventListener(\"load\",a,!1);else if(b.attachEvent)b.att"
    "achEvent(\"onload\",a);else{var d=b.onload;b.onload=function()"
    "{a.call(this);d&&d.call(this)}}};window.pagespeed=window.pag"
    "espeed||{};var q=window.pagespeed;function t(a,b,d,g,f){this"
    ".m=a;this.o=b;this.v=d;this.u=g;this.h=f;this.i=[];this.g=0}"
    "function u(a){a=document.querySelectorAll(a);if(a.length==0)"
    "return!1;var b=window.innerHeight||document.documentElement."
    "clientHeight;if(!b||!a[0].getBoundingClientRect)return!0;for"
    "(var d=window.pageYOffset||0,g=Math.min(a.length,10),f=0;f<g"
    ";++f){var c=a[f].getBoundingClientRect();if(c.width==0&&c.he"
    "ight==0||c.top+d<b)return!0}return a.length>g}\nt.prototype.j"
    "=function(a){for(var b=0;b<250&&this.g<this.h.length;++b,++t"
    "his.g)try{u(this.h[this.g])&&this.i.push(this.h[this.g])}cat"
    "ch(d){}this.g<this.h.length?window.setTimeout(this.j.bind(th"
    "is),0,a):a()};\nq.l=function(a,b,d,g,f){if(document.querySele"
    "ctor&&document.querySelectorAll&&Function.prototype.bind){va"
    "r c=new t(a,b,d,g,f);l(function(){window.setTimeout(function"
    "(){c.j(function(){var h=\"oh=\"+c.v+\"&n=\"+c.u;h+=\"&cs=\";for(va"
    "r e=c.i.length,n=Math.floor(Math.random()*e),r=!1,m=0;m<e;++"
    "m){var p=m>0?\",\":\"\";p+=encodeURIComponent(c.i[(n+m)%e]);if(h"
    ".length+p.length>131067){r=!0;break}h+=p}r&&(h+=\"&of=1\");q.c"
    "riticalCssBeaconData=h;e=c.m;n=c.o;if(window.XMLHttpRequest)"
    "var k=new XMLHttpRequest;else if(window.ActiveXObject)try{k="
    "\nnew ActiveXObject(\"Msxml2.XMLHTTP\")}catch(v){try{k=new Acti"
    "veXObject(\"Microsoft.XMLHTTP\")}catch(w){}}k&&(e=e+(e.indexOf"
    "(\"?\")==-1?\"?\":\"&\")+\"url=\"+encodeURIComponent(n),k.open(\"POST"
    "\",e),k.setRequestHeader(\"Content-Type\",\"application/x-www-fo"
    "rm-urlencoded\"),k.send(h))})},0)})}};q.criticalCssBeaconInit"
    "=q.l;})();\n";

}  // namespace net_instaweb
