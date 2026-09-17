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
// Automatically generated from delay_images_opt.js

namespace net_instaweb {

const char* JS_delay_images_opt =
    "(function(){function c(a,b,d){if(a.addEventListener)a.addEve"
    "ntListener(b,d,!1);else if(a.attachEvent)a.attachEvent(\"on\"+"
    "b,d);else{var e=a[\"on\"+b];a[\"on\"+b]=function(){d.call(this);"
    "e&&e.call(this)}}}var f=Date.now||function(){return+new Date"
    "};window.pagespeed=window.pagespeed||{};var g=window.pagespe"
    "ed;function k(){this.g=this.i=!1}k.prototype.h=function(a){f"
    "or(var b=0;b<a.length;++b){var d=a[b].getAttribute(\"data-pag"
    "espeed-high-res-src\");d&&a[b].setAttribute(\"src\",d)}};k.prot"
    "otype.replaceElementSrc=k.prototype.h;\nk.prototype.m=functio"
    "n(){if(this.i)this.g=!1;else{var a=document.body,b,d=0,e=thi"
    "s;\"ontouchstart\"in a?(c(a,\"touchstart\",function(){b=f()}),c("
    "a,\"touchend\",function(h){d=f();(h.changedTouches!=null&&h.ch"
    "angedTouches.length==2||h.touches!=null&&h.touches.length==2"
    "||d-b<500)&&l(e)})):c(window,\"click\",function(){l(e)});c(win"
    "dow,\"load\",function(){l(e)});this.i=!0}};k.prototype.registe"
    "rLazyLoadHighRes=k.prototype.m;function l(a){a.g||(a.j(),a.g"
    "=!0)}\nk.prototype.j=function(){this.h(document.getElementsBy"
    "TagName(\"img\"));this.h(document.getElementsByTagName(\"input\""
    "))};k.prototype.replaceWithHighRes=k.prototype.j;g.l=functio"
    "n(){g.delayImages=new k};g.delayImagesInit=g.l;})();\n";

}  // namespace net_instaweb
