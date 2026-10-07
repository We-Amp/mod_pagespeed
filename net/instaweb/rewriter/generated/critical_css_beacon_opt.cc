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
    "(function(){function m(b){var a=window;if(a.addEventListener"
    ")a.addEventListener(\"load\",b,!1);else if(a.attachEvent)a.att"
    "achEvent(\"onload\",b);else{var c=a.onload;a.onload=function()"
    "{b.call(this);c&&c.call(this)}}};window.pagespeed=window.pag"
    "espeed||{};var r=window.pagespeed;function u(b,a,c,h,e,f){th"
    "is.o=b;this.u=a;this.A=c;this.v=h;this.h=e;this.j=!!f;this.i"
    "=[];this.g=0}\nfunction v(b,a){a=document.querySelectorAll(a)"
    ";if(a.length==0)return!1;if(!b.j)return!0;b=window.innerHeig"
    "ht||document.documentElement.clientHeight;if(!b||!a[0].getBo"
    "undingClientRect)return!0;for(var c=window.pageYOffset||0,h="
    "Math.min(a.length,10),e=0;e<h;++e){var f=a[e].getBoundingCli"
    "entRect();if(f.width==0&&f.height==0||f.top+c<b)return!0}ret"
    "urn a.length>h}\nu.prototype.l=function(b){for(var a=0;a<250&"
    "&this.g<this.h.length;++a,++this.g)try{v(this,this.h[this.g]"
    ")&&this.i.push(this.h[this.g])}catch(c){this.j||this.i.push("
    "this.h[this.g])}this.g<this.h.length?window.setTimeout(this."
    "l.bind(this),0,b):b()};\nr.m=function(b,a,c,h,e,f){if(documen"
    "t.querySelector&&document.querySelectorAll&&Function.prototy"
    "pe.bind){var g=new u(b,a,c,h,e,f);m(function(){window.setTim"
    "eout(function(){g.l(function(){var k=\"oh=\"+g.A+\"&n=\"+g.v;k+="
    "\"&cs=\";for(var d=g.i.length,p=Math.floor(Math.random()*d),t="
    "!1,n=0;n<d;++n){var q=n>0?\",\":\"\";q+=encodeURIComponent(g.i[("
    "p+n)%d]);if(k.length+q.length>131067){t=!0;break}k+=q}t&&(k+"
    "=\"&of=1\");r.criticalCssBeaconData=k;d=g.o;p=g.u;if(window.XM"
    "LHttpRequest)var l=new XMLHttpRequest;else if(window.ActiveX"
    "Object)try{l=\nnew ActiveXObject(\"Msxml2.XMLHTTP\")}catch(w){t"
    "ry{l=new ActiveXObject(\"Microsoft.XMLHTTP\")}catch(x){}}l&&(d"
    "=d+(d.indexOf(\"?\")==-1?\"?\":\"&\")+\"url=\"+encodeURIComponent(p)"
    ",l.open(\"POST\",d),l.setRequestHeader(\"Content-Type\",\"applica"
    "tion/x-www-form-urlencoded\"),l.send(k))})},0)})}};r.critical"
    "CssBeaconInit=r.m;})();\n";

}  // namespace net_instaweb
