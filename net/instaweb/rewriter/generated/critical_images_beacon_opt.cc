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
    "(()=>{var h=function(t,i,e){var n;if(window.XMLHttpRequest)n"
    "=new XMLHttpRequest;else if(window.ActiveXObject)try{n=new A"
    "ctiveXObject(\"Msxml2.XMLHTTP\")}catch(o){try{n=new ActiveXObj"
    "ect(\"Microsoft.XMLHTTP\")}catch(c){}}if(!n)return!1;var a=t.i"
    "ndexOf(\"?\")==-1?\"?\":\"&\",s=t+a+\"url=\"+encodeURIComponent(i);r"
    "eturn n.open(\"POST\",s),n.setRequestHeader(\"Content-Type\",\"ap"
    "plication/x-www-form-urlencoded\"),n.send(e),!0},g=function(t"
    ",i,e){if(t.addEventListener)t.addEventListener(i,e,!1);else "
    "if(t.attachEvent)t.attachEvent(\"on\"+i,e);else{var n=t[\"on\"+i"
    "];t[\"on\"+i]=function(){e.call(this),n&&n.call(this)}}};var f"
    "=function(){var t=window.innerHeight||document.documentEleme"
    "nt.clientHeight||document.body.clientHeight,i=window.innerWi"
    "dth||document.documentElement.clientWidth||document.body.cli"
    "entWidth;return{height:t,width:i}};var m=Date.now||(function"
    "(){return+new Date});window.pagespeed=window.pagespeed||{};v"
    "ar r=window.pagespeed;r.CriticalImages=r.CriticalImages||{};"
    "r.CriticalImages.Beacon_=function(t,i,e,n,a){this.beaconUrl_"
    "=t,this.htmlUrl_=i,this.optionsHash_=e,this.nonce_=a,this.wi"
    "ndowSize_=f(),this.checkRenderedImageSizes_=n,this.imgLocati"
    "ons_={},this.criticalImages_=[],this.criticalImagesKeys_={}}"
    ";r.CriticalImages.Beacon_.prototype.elLocation_=function(t){"
    "var i=t.getBoundingClientRect(),e=document.body,n=\"pageXOffs"
    "et\"in window?window.pageXOffset:(document.documentElement||e"
    ".parentNode||e).scrollLeft,a=\"pageYOffset\"in window?window.p"
    "ageYOffset:(document.documentElement||e.parentNode||e).scrol"
    "lTop;return{top:i.top+a,left:i.left+n}};r.CriticalImages.Bea"
    "con_.prototype.isCritical_=function(t){if(t.offsetWidth<=0&&"
    "t.offsetHeight<=0)return!1;var i=this.elLocation_(t),e=i.top"
    ".toString()+\",\"+i.left;return this.imgLocations_.hasOwnPrope"
    "rty(e)?!1:(this.imgLocations_[e]=!0,i.top<=this.windowSize_."
    "height&&i.left<=this.windowSize_.width)};r.CriticalImages.is"
    "HtmlSpace_=function(t){return t==\" \"||t==\"\t\"||t==`\n`||t==\"\\f"
    "\"||t==\"\\r\"};r.CriticalImages.parseSrcSetUrls_=function(t){fo"
    "r(var i=[],e=0,n=t.length;;){for(;e<n&&(r.CriticalImages.isH"
    "tmlSpace_(t.charAt(e))||t.charAt(e)==\",\");)++e;if(e>=n)break"
    ";for(var a=e;a<n&&!r.CriticalImages.isHtmlSpace_(t.charAt(a)"
    ");)++a;var s=t.substring(e,a);e=a;for(var o=!0;s.charAt(s.le"
    "ngth-1)==\",\";)s=s.substring(0,s.length-1),o=!1;if(o)for(var "
    "c=!1;e<n;){var l=t.charAt(e);if(l==\"(\")c=!0;else if(l==\")\"&&"
    "c)c=!1;else if(l==\",\"&&!c)break;++e}i.push(s)}return i};r.Cr"
    "iticalImages.absolutifyUrl_=function(t){var i=document.creat"
    "eElement(\"a\");return i.href=t,i.href};r.CriticalImages.Beaco"
    "n_.prototype.imageKey_=function(t){var i=t.getAttribute(\"dat"
    "a-pagespeed-url-hash\");if(i)return i;var e=t.getAttribute(\"d"
    "ata-pagespeed-srcset-url-hashes\");if(!e)return null;var n=t."
    "currentSrc;if(!n)return null;for(var a=r.CriticalImages.pars"
    "eSrcSetUrls_(t.getAttribute(\"srcset\")||\"\"),s=e.split(\",\"),o="
    "0;o<a.length&&o<s.length;++o)if(s[o]&&r.CriticalImages.absol"
    "utifyUrl_(a[o])==n)return s[o];return null};r.CriticalImages"
    ".Beacon_.prototype.insertIfImageIsCritical_=function(t){var "
    "i=this.imageKey_(t);i&&!(i in this.criticalImagesKeys_)&&thi"
    "s.isCritical_(t)&&(this.criticalImages_.push(i),this.critica"
    "lImagesKeys_[i]=!0)};r.CriticalImages.Beacon_.prototype.chec"
    "kImageForCriticality=function(t){t.getBoundingClientRect&&th"
    "is.insertIfImageIsCritical_(t)};r.CriticalImages.checkImageF"
    "orCriticality=function(t){r.CriticalImages.beaconObj_.checkI"
    "mageForCriticality(t)};r.CriticalImages.checkCriticalImages="
    "function(){r.CriticalImages.beaconObj_.checkCriticalImages_("
    ")};r.CriticalImages.Beacon_.prototype.checkCriticalImages_=f"
    "unction(){this.imgLocations_={};for(var t=[\"IMG\",\"INPUT\"],i="
    "[],e=0;e<t.length;++e)i=i.concat(Array.from(document.getElem"
    "entsByTagName(t[e])));if(i.length!=0&&i[0].getBoundingClient"
    "Rect){for(var e=0,n;n=i[e];++e)this.insertIfImageIsCritical_"
    "(n);var a=\"oh=\"+this.optionsHash_;this.nonce_&&(a+=\"&n=\"+thi"
    "s.nonce_);var s=this.criticalImages_.length!=0;if(s){a+=\"&ci"
    "=\"+encodeURIComponent(this.criticalImages_[0]);for(var e=1;e"
    "<this.criticalImages_.length;++e){var o=\",\"+encodeURICompone"
    "nt(this.criticalImages_[e]);a.length+o.length<=131072&&(a+=o"
    ")}}if(this.checkRenderedImageSizes_){var o=\"&rd=\"+encodeURIC"
    "omponent(JSON.stringify(this.getImageRenderedMap()));a.lengt"
    "h+o.length<=131072&&(a+=o),s=!0}r.CriticalImages.beaconData_"
    "=a,s&&h(this.beaconUrl_,this.htmlUrl_,a)}};r.CriticalImages."
    "Beacon_.prototype.getImageRenderedMap=function(){var t={},i="
    "document.getElementsByTagName(\"IMG\");if(i.length==0)return{}"
    ";var e=i[0];if(!(\"naturalWidth\"in e)||!(\"naturalHeight\"in e)"
    ")return{};for(var n=0;e=i[n];++n){var a=this.imageKey_(e);a&"
    "&(!(a in t)&&e.width>0&&e.height>0&&e.naturalWidth>0&&e.natu"
    "ralHeight>0||a in t&&e.width>=t[a].rw&&e.height>=t[a].rh)&&("
    "t[a]={rw:e.width,rh:e.height,ow:e.naturalWidth,oh:e.naturalH"
    "eight})}return t};r.CriticalImages.beaconData_=\"\";r.Critical"
    "Images.getBeaconData=function(){return r.CriticalImages.beac"
    "onData_};r.CriticalImages.Run=function(t,i,e,n,a,s){var o=ne"
    "w r.CriticalImages.Beacon_(t,i,e,a,s);if(r.CriticalImages.be"
    "aconObj_=o,n){var c=function(){window.setTimeout(function(){"
    "o.checkCriticalImages_()},0)};g(window,\"load\",c)}};})();\n";

}  // namespace net_instaweb
