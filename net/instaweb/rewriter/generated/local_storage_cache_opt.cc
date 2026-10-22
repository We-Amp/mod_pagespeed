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
// Automatically generated from local_storage_cache_opt.js

namespace net_instaweb {

const char* JS_local_storage_cache_opt =
    "(()=>{var w=function(e,a,t){if(e.addEventListener)e.addEvent"
    "Listener(a,t,!1);else if(e.attachEvent)e.attachEvent(\"on\"+a,"
    "t);else{var n=e[\"on\"+a];e[\"on\"+a]=function(){t.call(this),n&"
    "&n.call(this)}}};var v=Date.now||(function(){return+new Date"
    "});window.pagespeed=window.pagespeed||{};var r=window.pagesp"
    "eed;r.LocalStorageCache=function(){this.regenerate_cookie_=!"
    "0};r.LocalStorageCache.prototype.hasExpired=function(e){var "
    "a=parseInt(e.substring(0,e.indexOf(\" \")),10);return!isNaN(a)"
    "&&a<=v()};r.LocalStorageCache.prototype.getData=function(e){"
    "var a=e.indexOf(\" \"),t=e.indexOf(\" \",a+1);return e.substring"
    "(t+1)};r.LocalStorageCache.prototype.replaceLastScript=funct"
    "ion(e){var a=document.getElementsByTagName(\"script\"),t=a[a.l"
    "ength-1];t.parentNode.replaceChild(e,t)};r.LocalStorageCache"
    ".prototype.inlineCss=function(e){var a=null;try{a=window.loc"
    "alStorage.getItem(\"pagespeed_lsc_url:\"+e)}catch(n){}var t=do"
    "cument.createElement(a?\"style\":\"link\");a&&!this.hasExpired(a"
    ")?(t.type=\"text/css\",t.appendChild(document.createTextNode(t"
    "his.getData(a)))):(t.rel=\"stylesheet\",t.href=e,this.regenera"
    "te_cookie_=!0),this.replaceLastScript(t)};r.LocalStorageCach"
    "e.prototype.inlineImg=function(e,a){var t=null;try{t=window."
    "localStorage.getItem(\"pagespeed_lsc_url:\"+e+\" pagespeed_lsc_"
    "hash:\"+a)}catch(c){}var n=document.createElement(\"img\");t&&!"
    "this.hasExpired(t)?n.src=this.getData(t):(n.src=e,this.regen"
    "erate_cookie_=!0);for(var o=2,i=arguments.length;o<i;++o){va"
    "r s=arguments[o].indexOf(\"=\");n.setAttribute(arguments[o].su"
    "bstring(0,s),arguments[o].substring(s+1))}this.replaceLastSc"
    "ript(n)};r.LocalStorageCache.prototype.processTags_=function"
    "(e,a,t){for(var n=document.getElementsByTagName(e),o=0,i=n.l"
    "ength;o<i;++o){var s=n[o],c=s.getAttribute(\"data-pagespeed-l"
    "sc-hash\"),l=s.getAttribute(\"data-pagespeed-lsc-url\");if(c&&l"
    "){var d=\"pagespeed_lsc_url:\"+l;a&&(d+=\" pagespeed_lsc_hash:\""
    "+c);var p=s.getAttribute(\"data-pagespeed-lsc-expiry\"),f=p?ne"
    "w Date(p).getTime():\"\",u=t(s);if(!u){var g=null;try{g=window"
    ".localStorage.getItem(d)}catch(h){}g&&(u=this.getData(g))}if"
    "(u)try{window.localStorage.setItem(d,f+\" \"+c+\" \"+u),this.reg"
    "enerate_cookie_=!0}catch(h){}}}};r.LocalStorageCache.prototy"
    "pe.saveInlinedData_=function(){this.processTags_(\"img\",!0,fu"
    "nction(e){return e.src}),this.processTags_(\"style\",!1,functi"
    "on(e){return e.firstChild?e.firstChild.nodeValue:null})};r.L"
    "ocalStorageCache.prototype.generateCookie_=function(){if(thi"
    "s.regenerate_cookie_){var e=[],a=[],t=0,n=v(),o=0;try{o=wind"
    "ow.localStorage.length}catch(h){}for(var i=0,s=o;i<s;++i)try"
    "{var c=window.localStorage.key(i);if(c.indexOf(\"pagespeed_ls"
    "c_url:\"))continue;var l=window.localStorage.getItem(c),d=l.i"
    "ndexOf(\" \"),p=parseInt(l.substring(0,d),10);if(!isNaN(p))if("
    "p<=n){e.push(c);continue}else(p<t||t==0)&&(t=p);var f=l.inde"
    "xOf(\" \",d+1),u=l.substring(d+1,f);a.push(u)}catch(h){continu"
    "e}var g=\"\";t&&(g=\"; expires=\"+new Date(t).toUTCString()),doc"
    "ument.cookie=\"_GPSLSC=\"+a.join(\"!\")+\";path=/\"+g;for(var i=0,"
    "s=e.length;i<s;++i)try{window.localStorage.removeItem(e[i])}"
    "catch(m){}this.regenerate_cookie_=!1}};r.localStorageCacheIn"
    "it=function(){var e=new r.LocalStorageCache;r.localStorageCa"
    "che=e,w(window,\"load\",function(){e.saveInlinedData_()}),w(wi"
    "ndow,\"load\",function(){e.generateCookie_()})};})();\n";

}  // namespace net_instaweb
