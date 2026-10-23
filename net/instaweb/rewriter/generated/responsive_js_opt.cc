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
    "(()=>{var p=function(i){var t=Number(i);return t==0&&/^[\\s\\x"
    "a0]*$/.test(i)?NaN:t},c=function(){return window.devicePixel"
    "Ratio!==void 0?window.devicePixelRatio:1};window.pagespeed=w"
    "indow.pagespeed||{};var s=window.pagespeed;s.ResponsiveImage"
    "Candidate=function(i,t){this.resolution=i,this.url=t};s.Resp"
    "onsiveImage=function(i){this.img=i,this.currentResolution=0,"
    "this.availableResolutions=[]};s.Responsive=function(){this.a"
    "llImages_=[]};s.Responsive.updateImgSrc_=function(i,t){var n"
    "=new Image;n.onload=function(){i.src=t},n.src=t};s.Responsiv"
    "eImage.prototype.responsiveResize=function(i){if(i>this.curr"
    "entResolution){for(var t=this.availableResolutions.length,n="
    "0;n<t;++n)if(i<=this.availableResolutions[n].resolution){thi"
    "s.currentResolution=this.availableResolutions[n].resolution,"
    "s.Responsive.updateImgSrc_(this.img,this.availableResolution"
    "s[n].url);break}}};s.Responsive.prototype.computeDevicePixel"
    "RatioWithZoom=function(){var i=document.documentElement.clie"
    "ntWidth/window.innerWidth;return c()*i};s.Responsive.prototy"
    "pe.responsiveResize=function(){for(var i=this.computeDeviceP"
    "ixelRatioWithZoom(),t=this.allImages_.length,n=0;n<t;++n)thi"
    "s.allImages_[n].responsiveResize(i)};s.Responsive.search_=fu"
    "nction(i,t){var n=i.search(t);return n==-1?i.length:n};var g"
    "=/[ \\t\\n\\f\\r]/,h=/[^ \\t\\n\\f\\r]/,d=/[ \\t\\n\\f\\r,]/,R=/[^ \\t\\n\\"
    "f\\r,]/;s.Responsive.prototype.parseSrcset=function(i,t,n){va"
    "r a=new s.ResponsiveImage(i),r=!1,e=n,o=s.Responsive.search_"
    "(e,R);for(e=e.slice(o);e.length>0;){o=s.Responsive.search_(e"
    ",g);var v=e.slice(0,o);if(e=e.slice(o),v[v.length-1]==\",\")re"
    "turn null;o=s.Responsive.search_(e,h),e=e.slice(o),o=s.Respo"
    "nsive.search_(e,d);var l=e.slice(0,o);if(e=e.slice(o),l.leng"
    "th>1&&l[l.length-1]==\"x\"){var u=p(l.slice(0,-1));if(isNaN(u)"
    ")return null;a.availableResolutions.push(new s.ResponsiveIma"
    "geCandidate(u,v)),u==1&&(r=!0)}else return null;if(o=s.Respo"
    "nsive.search_(e,h),e=e.slice(o),e.length>0&&e[0]!=\",\")return"
    " null;e=e.slice(1),o=s.Responsive.search_(e,R),e=e.slice(o)}"
    "return!r&&t&&a.availableResolutions.push(new s.ResponsiveIma"
    "geCandidate(1,t)),a.availableResolutions.sort(function(f,m){"
    "return f.resolution-m.resolution}),a};s.Responsive.prototype"
    ".init=function(){for(var i=document.getElementsByTagName(\"IM"
    "G\"),t=0,n;n=i[t];++t){var a=n.getAttribute(\"src\"),r=n.getAtt"
    "ribute(\"srcset\");if(r){var e=this.parseSrcset(n,a,r);e!=null"
    "&&this.allImages_.push(e)}}window.addEventListener(\"resize\","
    "this.responsiveResize.bind(this)),window.addEventListener(\"t"
    "ouchmove\",o=>{o.touches.length>1&&this.responsiveResize()}),"
    "this.responsiveResize()};s.responsiveInstance=new s.Responsi"
    "ve;s.responsiveInstance.init();})();\n";

}  // namespace net_instaweb
