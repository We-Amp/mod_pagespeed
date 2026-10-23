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
    "(()=>{var s=function(e,t,n){if(e.addEventListener)e.addEvent"
    "Listener(t,n,!1);else if(e.attachEvent)e.attachEvent(\"on\"+t,"
    "n);else{var a=e[\"on\"+t];e[\"on\"+t]=function(){n.call(this),a&"
    "&a.call(this)}}};var d=Date.now||(function(){return+new Date"
    "});window.pagespeed=window.pagespeed||{};var i=window.pagesp"
    "eed;i.DelayImages=function(){this.lazyLoadHighResHandlersReg"
    "istered_=!1,this.highResReplaced_=!1};i.DelayImages.prototyp"
    "e.replaceElementSrc=function(e){for(var t=0;t<e.length;++t){"
    "var n=e[t].getAttribute(\"data-pagespeed-high-res-src\");n&&e["
    "t].setAttribute(\"src\",n)}};i.DelayImages.prototype.registerL"
    "azyLoadHighRes=function(){if(this.lazyLoadHighResHandlersReg"
    "istered_){this.highResReplaced_=!1;return}var e=document.bod"
    "y,t=500,n,a=0,r=this;this.highResReplaced=!1,\"ontouchstart\"i"
    "n e?(s(e,\"touchstart\",function(o){n=d()}),s(e,\"touchend\",fun"
    "ction(o){a=d(),(o.changedTouches!=null&&o.changedTouches.len"
    "gth==2||o.touches!=null&&o.touches.length==2||a-n<t)&&r.load"
    "HighRes()})):s(window,\"click\",function(o){r.loadHighRes()}),"
    "s(window,\"load\",function(o){r.loadHighRes()}),this.lazyLoadH"
    "ighResHandlersRegistered_=!0};i.DelayImages.prototype.loadHi"
    "ghRes=function(){this.highResReplaced_||(this.replaceWithHig"
    "hRes(),this.highResReplaced_=!0)};i.DelayImages.prototype.re"
    "placeWithHighRes=function(){this.replaceElementSrc(document."
    "getElementsByTagName(\"img\")),this.replaceElementSrc(document"
    ".getElementsByTagName(\"input\"))};i.delayImagesInit=function("
    "){var e=new i.DelayImages;i.delayImages=e};})();\n";

}  // namespace net_instaweb
