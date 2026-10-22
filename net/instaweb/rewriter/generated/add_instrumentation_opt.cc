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
// Automatically generated from add_instrumentation_opt.js

namespace net_instaweb {

const char* JS_add_instrumentation_opt =
    "(()=>{window.pagespeed=window.pagespeed||{};var a=window.pag"
    "espeed;a.AddInstrumentation=function(t,i,e){this.beaconUrlPr"
    "efix_=t,this.extraParams_=i,this.htmlUrl_=e,this.sent_=!1,th"
    "is.lcpMs_=-1,this.clsSession_={sum:0,firstTime:0,lastTime:0}"
    ",this.cls_=0,this.clsObserved_=!1,this.inpMs_=-1,this.observ"
    "ers_=[],this.initObservers_()};a.AddInstrumentation.prototyp"
    "e.observe_=function(t,i,e){var r=window.PerformanceObserver;"
    "if(!r||!r.supportedEntryTypes||r.supportedEntryTypes.indexOf"
    "(t)==-1)return!1;try{var n=new r(function(o){i(o.getEntries("
    "))}),s=e||{};return s.type=t,s.buffered=!0,n.observe(s),this"
    ".observers_.push({observer:n,handler:i}),!0}catch(o){return!"
    "1}};a.AddInstrumentation.prototype.initObservers_=function()"
    "{var t=this;this.observe_(\"largest-contentful-paint\",functio"
    "n(e){for(var r=0;r<e.length;r++){var n=e[r];t.lcpMs_=Math.ro"
    "und(n.renderTime||n.loadTime||n.startTime)}}),this.clsObserv"
    "ed_=this.observe_(\"layout-shift\",function(e){for(var r=0;r<e"
    ".length;r++){var n=e[r];if(!n.hadRecentInput){var s=t.clsSes"
    "sion_;s.sum>0&&n.startTime-s.lastTime<1e3&&n.startTime-s.fir"
    "stTime<5e3?(s.sum+=n.value,s.lastTime=n.startTime):(s.sum=n."
    "value,s.firstTime=s.lastTime=n.startTime),s.sum>t.cls_&&(t.c"
    "ls_=s.sum)}}});var i=function(e){for(var r=0;r<e.length;r++)"
    "{var n=e[r];if(!(n.entryType==\"event\"&&!n.interactionId)){va"
    "r s=Math.round(n.duration);s>t.inpMs_&&(t.inpMs_=s)}}};this."
    "observe_(\"event\",i,{durationThreshold:40}),this.observe_(\"fi"
    "rst-input\",i)};a.AddInstrumentation.prototype.rearm_=functio"
    "n(){for(var t=0;t<this.observers_.length;t++)try{this.observ"
    "ers_[t].observer.disconnect()}catch(i){}this.observers_=[],t"
    "his.sent_=!1,this.lcpMs_=-1,this.clsSession_={sum:0,firstTim"
    "e:0,lastTime:0},this.cls_=0,this.inpMs_=-1,this.initObserver"
    "s_()};a.AddInstrumentation.prototype.buildBody_=function(){v"
    "ar t=[],i=window.performance,e=null;if(i&&i.getEntriesByType"
    "){var r=i.getEntriesByType(\"navigation\");r&&r.length>0&&(e=r"
    "[0])}if(e){var n=Math.round(e.loadEventStart);n>0&&(t.push(\""
    "ets=load:\"+n),t.push(\"rload=\"+n)),t.push(\"nav=\"+Math.round(e"
    ".fetchStart)),t.push(\"dns=\"+Math.round(e.domainLookupEnd-e.d"
    "omainLookupStart)),t.push(\"connect=\"+Math.round(e.connectEnd"
    "-e.connectStart)),t.push(\"req_start=\"+Math.round(e.requestSt"
    "art)),t.push(\"c_ttfb=\"+Math.round(e.responseStart)),t.push(\""
    "dwld=\"+Math.round(e.responseEnd-e.responseStart)),t.push(\"do"
    "m_c=\"+Math.round(e.domContentLoadedEventStart)),e.type&&t.pu"
    "sh(\"nt=\"+e.type)}else window.mod_pagespeed_start&&t.push(\"et"
    "s=load:\"+(Number(new Date)-window.mod_pagespeed_start));this"
    ".lcpMs_>=0&&t.push(\"lcp=\"+this.lcpMs_),this.clsObserved_&&t."
    "push(\"cls=\"+Math.round(this.cls_*1e3)),this.inpMs_>=0&&t.pus"
    "h(\"inp=\"+this.inpMs_),t.push(\"dpr=\"+window.devicePixelRatio)"
    ";var s=t.join(\"&\");return this.extraParams_!=\"\"&&(s+=this.ex"
    "traParams_),a.getResourceTimingData&&(s+=a.getResourceTiming"
    "Data()),document.referrer&&(s+=\"&ref=\"+encodeURIComponent(do"
    "cument.referrer)),s};a.AddInstrumentation.prototype.send_=fu"
    "nction(){if(!this.sent_){this.sent_=!0;for(var t=0;t<this.ob"
    "servers_.length;t++){var i=this.observers_[t];try{i.handler("
    "i.observer.takeRecords()),i.observer.disconnect()}catch(s){}"
    "}var e=this.buildBody_(),r=this.beaconUrlPrefix_;r+=r.indexO"
    "f(\"?\")==-1?\"?\":\"&\",r+=\"url=\"+encodeURIComponent(this.htmlUrl"
    "_);var n=window.navigator;if(n&&n.sendBeacon)try{if(n.sendBe"
    "acon(r,e))return}catch(s){}new Image().src=r+\"&\"+e}};a.addIn"
    "strumentationInit=function(t,i,e){if(window.parent==window){"
    "var r=new a.AddInstrumentation(t,i,e);document.addEventListe"
    "ner(\"visibilitychange\",function(){document.visibilityState=="
    "\"hidden\"&&r.send_()}),window.addEventListener(\"pagehide\",fun"
    "ction(){r.send_()}),window.addEventListener(\"pageshow\",funct"
    "ion(n){n.persisted&&r.rearm_()})}};})();\n";

}  // namespace net_instaweb
