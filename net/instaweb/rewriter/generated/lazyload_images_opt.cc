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
// Automatically generated from lazyload_images_opt.js

namespace net_instaweb {

const char* JS_lazyload_images_opt =
    "(()=>{var n=function(t,e,o){if(t.addEventListener)t.addEvent"
    "Listener(e,o,!1);else if(t.attachEvent)t.attachEvent(\"on\"+e,"
    "o);else{var a=t[\"on\"+e];t[\"on\"+e]=function(){o.call(this),a&"
    "&a.call(this)}}};var l=Date.now||(function(){return+new Date"
    "});window.pagespeed=window.pagespeed||{};var i=window.pagesp"
    "eed;i.LazyloadImages=function(t){this.deferred_=[],this.buff"
    "er_=0,this.force_load_=!1,this.blank_image_src_=t,this.scrol"
    "l_timer_=null,this.last_scroll_time_=0,this.min_scroll_time_"
    "=200,this.onload_done_=!1,this.imgs_to_load_before_beaconing"
    "_=0};i.LazyloadImages.prototype.countDeferredImgs_=function("
    "){for(var t=0,e=document.getElementsByTagName(\"img\"),o=0,a;a"
    "=e[o];o++)a.src.indexOf(this.blank_image_src_)!=-1&&this.has"
    "Attribute_(a,\"data-pagespeed-lazy-src\")&&t++;return t};i.Laz"
    "yloadImages.prototype.viewport_=function(){var t=0;typeof wi"
    "ndow.pageYOffset==\"number\"?t=window.pageYOffset:document.bod"
    "y&&document.body.scrollTop?t=document.body.scrollTop:documen"
    "t.documentElement&&document.documentElement.scrollTop&&(t=do"
    "cument.documentElement.scrollTop);var e=window.innerHeight||"
    "document.documentElement.clientHeight||document.body.clientH"
    "eight;return{top:t,bottom:t+e,height:e}};i.LazyloadImages.pr"
    "ototype.compute_top_=function(t){var e=t.getAttribute(\"data-"
    "pagespeed-lazy-position\");if(e)return parseInt(e,0);e=t.offs"
    "etTop;var o=t.offsetParent;return o&&(e+=this.compute_top_(o"
    ")),e=Math.max(e,0),t.setAttribute(\"data-pagespeed-lazy-posit"
    "ion\",e),e};i.LazyloadImages.prototype.offset_=function(t){va"
    "r e=this.compute_top_(t);return{top:e,bottom:e+t.offsetHeigh"
    "t}};i.LazyloadImages.prototype.isVisible_=function(t){if(!th"
    "is.onload_done_&&(t.offsetHeight==0||t.offsetWidth==0))retur"
    "n!1;var e=this.viewport_(),o=t.getBoundingClientRect(),a,r;i"
    "f(o)a=o.top-e.height,r=o.bottom;else{var s=this.offset_(t);a"
    "=s.top-e.bottom,r=s.bottom-e.top}return a<=this.buffer_&&r+t"
    "his.buffer_>=0};i.LazyloadImages.prototype.loadIfVisibleAndM"
    "aybeBeacon=function(t){this.overrideAttributeFunctionsIntern"
    "al_(t);var e=this;window.setTimeout(function(){var o=t.getAt"
    "tribute(\"data-pagespeed-lazy-src\");if(o)if((e.force_load_||e"
    ".isVisible_(t))&&t.src.indexOf(e.blank_image_src_)!=-1){var "
    "a=t.parentNode,r=t.nextSibling;if(a&&a.removeChild(t),t._get"
    "Attribute&&(t.getAttribute=t._getAttribute),t.removeAttribut"
    "e(\"onload\"),t.tagName&&t.tagName==\"IMG\"&&i.CriticalImages){v"
    "ar s=!1,d=function(){s||(s=!0,e.onload_done_&&(e.imgs_to_loa"
    "d_before_beaconing_--,e.imgs_to_load_before_beaconing_==0&&i"
    ".CriticalImages.checkCriticalImages()))};n(t,\"load\",function"
    "(f){i.CriticalImages.checkImageForCriticality(this),d()}),n("
    "t,\"error\",function(f){d()})}t.removeAttribute(\"data-pagespee"
    "d-lazy-src\"),t.removeAttribute(\"data-pagespeed-lazy-replaced"
    "-functions\"),a&&a.insertBefore(t,r);var c=t.getAttribute(\"da"
    "ta-pagespeed-lazy-srcset\");c&&(t.srcset=c,t.removeAttribute("
    "\"data-pagespeed-lazy-srcset\")),t.src=o}else e.deferred_.push"
    "(t)},0)};i.LazyloadImages.prototype.loadAllImages=function()"
    "{this.force_load_=!0,this.loadVisible_();for(var t=document."
    "getElementsByTagName(\"img\"),e=0,o;o=t[e];e++)this.hasAttribu"
    "te_(o,\"data-pagespeed-lazy-src\")&&this.loadIfVisibleAndMaybe"
    "Beacon(o)};i.LazyloadImages.prototype.loadVisible_=function("
    "){var t=this.deferred_,e=t.length;this.deferred_=[];for(var "
    "o=0;o<e;++o)this.loadIfVisibleAndMaybeBeacon(t[o])};i.Lazylo"
    "adImages.prototype.hasAttribute_=function(t,e){return t.getA"
    "ttribute_?t.getAttribute_(e)!=null:t.getAttribute(e)!=null};"
    "i.LazyloadImages.prototype.overrideAttributeFunctions=functi"
    "on(){for(var t=document.getElementsByTagName(\"img\"),e=0,o;o="
    "t[e];e++)this.hasAttribute_(o,\"data-pagespeed-lazy-src\")&&th"
    "is.overrideAttributeFunctionsInternal_(o)};i.LazyloadImages."
    "prototype.overrideAttributeFunctionsInternal_=function(t){va"
    "r e=this;this.hasAttribute_(t,\"data-pagespeed-lazy-replaced-"
    "functions\")||(t._getAttribute=t.getAttribute,t.getAttribute="
    "function(o){return o.toLowerCase()==\"src\"&&e.hasAttribute_(t"
    "his,\"data-pagespeed-lazy-src\")&&(o=\"data-pagespeed-lazy-src\""
    "),this._getAttribute(o)},t.setAttribute(\"data-pagespeed-lazy"
    "-replaced-functions\",\"1\"))};i.lazyLoadInit=function(t,e){var"
    " o=new i.LazyloadImages(e);i.lazyLoadImages=o;var a=function"
    "(){o.onload_done_=!0,o.force_load_=t,o.buffer_=200,i.Critica"
    "lImages&&(o.imgs_to_load_before_beaconing_=o.countDeferredIm"
    "gs_(),o.imgs_to_load_before_beaconing_==0&&i.CriticalImages."
    "checkCriticalImages()),o.loadVisible_()};n(window,\"load\",a),"
    "e.indexOf(\"data:\")!=0&&(new Image().src=e);var r=function(){"
    "if(!(o.onload_done_&&t)&&!o.scroll_timer_){var s=new Date()."
    "getTime(),d=o.min_scroll_time_;s-o.last_scroll_time_>o.min_s"
    "croll_time_&&(d=0),o.scroll_timer_=window.setTimeout(functio"
    "n(){o.last_scroll_time_=new Date().getTime(),o.loadVisible_("
    "),o.scroll_timer_=null},d)}};n(window,\"scroll\",r),n(window,\""
    "resize\",r)};})();\n";

}  // namespace net_instaweb
