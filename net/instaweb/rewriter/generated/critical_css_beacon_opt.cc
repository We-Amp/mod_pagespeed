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
    "(()=>{var h=function(t,e,n){var i;if(window.XMLHttpRequest)i"
    "=new XMLHttpRequest;else if(window.ActiveXObject)try{i=new A"
    "ctiveXObject(\"Msxml2.XMLHTTP\")}catch(s){try{i=new ActiveXObj"
    "ect(\"Microsoft.XMLHTTP\")}catch(a){}}if(!i)return!1;var r=t.i"
    "ndexOf(\"?\")==-1?\"?\":\"&\",o=t+r+\"url=\"+encodeURIComponent(e);r"
    "eturn i.open(\"POST\",o),i.setRequestHeader(\"Content-Type\",\"ap"
    "plication/x-www-form-urlencoded\"),i.send(n),!0},l=function(t"
    ",e,n){if(t.addEventListener)t.addEventListener(e,n,!1);else "
    "if(t.attachEvent)t.attachEvent(\"on\"+e,n);else{var i=t[\"on\"+e"
    "];t[\"on\"+e]=function(){n.call(this),i&&i.call(this)}}};var f"
    "=Date.now||(function(){return+new Date});window.pagespeed=wi"
    "ndow.pagespeed||{};var c=window.pagespeed;c.CriticalCssBeaco"
    "n=function(t,e,n,i,r,o){this.MAXITERS_=250,this.MAXMEASURES_"
    "=10,this.beaconUrl_=t,this.htmlUrl_=e,this.optionsHash_=n,th"
    "is.nonce_=i,this.selectors_=r,this.aboveTheFoldOnly_=!!o,thi"
    "s.criticalSelectors_=[],this.idx_=0};c.CriticalCssBeacon.pro"
    "totype.sendBeacon_=function(){var t=\"oh=\"+this.optionsHash_+"
    "\"&n=\"+this.nonce_;t+=\"&cs=\";for(var e=this.criticalSelectors"
    "_.length,n=Math.floor(Math.random()*e),i=131067,r=!1,o=0;o<e"
    ";++o){var s=o>0?\",\":\"\";if(s+=encodeURIComponent(this.critica"
    "lSelectors_[(n+o)%e]),t.length+s.length>i){r=!0;break}t+=s}r"
    "&&(t+=\"&of=1\"),c.criticalCssBeaconData=t,h(this.beaconUrl_,t"
    "his.htmlUrl_,t)};c.CriticalCssBeacon.prototype.isSelectorCri"
    "tical_=function(t){var e=document.querySelectorAll(t);if(e.l"
    "ength==0)return!1;if(!this.aboveTheFoldOnly_)return!0;var n="
    "window.innerHeight||document.documentElement.clientHeight;if"
    "(!n||!e[0].getBoundingClientRect)return!0;for(var i=window.p"
    "ageYOffset||0,r=Math.min(e.length,this.MAXMEASURES_),o=0;o<r"
    ";++o){var s=e[o].getBoundingClientRect();if(s.width==0&&s.he"
    "ight==0||s.top+i<n)return!0}return e.length>r};c.CriticalCss"
    "Beacon.prototype.checkCssSelectors_=function(t){for(var e=0;"
    "e<this.MAXITERS_&&this.idx_<this.selectors_.length;++e,++thi"
    "s.idx_)try{this.isSelectorCritical_(this.selectors_[this.idx"
    "_])&&this.criticalSelectors_.push(this.selectors_[this.idx_]"
    ")}catch(n){this.aboveTheFoldOnly_||this.criticalSelectors_.p"
    "ush(this.selectors_[this.idx_]);continue}this.idx_<this.sele"
    "ctors_.length?window.setTimeout(this.checkCssSelectors_.bind"
    "(this),0,t):t()};c.criticalCssBeaconInit=function(t,e,n,i,r,"
    "o){if(!(!document.querySelector||!document.querySelectorAll|"
    "|!Function.prototype.bind)){var s=new c.CriticalCssBeacon(t,"
    "e,n,i,r,o),a=function(){window.setTimeout(function(){s.check"
    "CssSelectors_(function(){s.sendBeacon_()})},0)};l(window,\"lo"
    "ad\",a)}};})();\n";

}  // namespace net_instaweb
