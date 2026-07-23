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
    "(function(){function e(b){var a=window;if(a.addEventListener"
    ")a.addEventListener(\"load\",b,!1);else if(a.attachEvent)a.att"
    "achEvent(\"onload\",b);else{var c=a.onload;a.onload=function()"
    "{b.call(this);c&&c.call(this)}}}var p=Date.now||function(){r"
    "eturn+new Date};window.pagespeed=window.pagespeed||{};var r="
    "window.pagespeed;function t(){this.g=!0}t.prototype.i=functi"
    "on(b){b=parseInt(b.substring(0,b.indexOf(\" \")),10);return!is"
    "NaN(b)&&b<=p()};t.prototype.hasExpired=t.prototype.i;t.proto"
    "type.h=function(b){return b.substring(b.indexOf(\" \",b.indexO"
    "f(\" \")+1)+1)};t.prototype.getData=t.prototype.h;t.prototype."
    "j=function(b){var a=document.getElementsByTagName(\"script\");"
    "a=a[a.length-1];a.parentNode.replaceChild(b,a)};t.prototype."
    "replaceLastScript=t.prototype.j;\nt.prototype.l=function(b){v"
    "ar a=null;try{a=window.localStorage.getItem(\"pagespeed_lsc_u"
    "rl:\"+b)}catch(f){}var c=document.createElement(a?\"style\":\"li"
    "nk\");a&&!this.i(a)?(c.type=\"text/css\",c.appendChild(document"
    ".createTextNode(this.h(a)))):(c.rel=\"stylesheet\",c.href=b,th"
    "is.g=!0);this.j(c)};t.prototype.inlineCss=t.prototype.l;\nt.p"
    "rototype.o=function(b,a){var c=null;try{c=window.localStorag"
    "e.getItem(\"pagespeed_lsc_url:\"+b+\" pagespeed_lsc_hash:\"+a)}c"
    "atch(d){}var f=document.createElement(\"img\");c&&!this.i(c)?f"
    ".src=this.h(c):(f.src=b,this.g=!0);c=2;for(var h=arguments.l"
    "ength;c<h;++c){var k=arguments[c].indexOf(\"=\");f.setAttribut"
    "e(arguments[c].substring(0,k),arguments[c].substring(k+1))}t"
    "his.j(f)};t.prototype.inlineImg=t.prototype.o;\nfunction u(b,"
    "a,c,f){a=document.getElementsByTagName(a);for(var h=0,k=a.le"
    "ngth;h<k;++h){var d=a[h],n=d.getAttribute(\"data-pagespeed-ls"
    "c-hash\"),g=d.getAttribute(\"data-pagespeed-lsc-url\");if(n&&g)"
    "{g=\"pagespeed_lsc_url:\"+g;c&&(g+=\" pagespeed_lsc_hash:\"+n);v"
    "ar l=d.getAttribute(\"data-pagespeed-lsc-expiry\");l=l?(new Da"
    "te(l)).getTime():\"\";d=f(d);if(!d){var m=null;try{m=window.lo"
    "calStorage.getItem(g)}catch(q){}m&&(d=b.h(m))}if(d)try{windo"
    "w.localStorage.setItem(g,l+\" \"+n+\" \"+d),b.g=!0}catch(q){}}}}"
    "\nfunction v(b){u(b,\"img\",!0,function(a){return a.src});u(b,\""
    "style\",!1,function(a){return a.firstChild?a.firstChild.nodeV"
    "alue:null})}\nr.m=function(){var b=new t;r.localStorageCache="
    "b;e(function(){v(b)});e(function(){if(b.g){var a=[],c=[],f=0"
    ",h=p(),k=0;try{k=window.localStorage.length}catch(q){}for(va"
    "r d=0;d<k;++d)try{var n=window.localStorage.key(d);if(!n.ind"
    "exOf(\"pagespeed_lsc_url:\")){var g=window.localStorage.getIte"
    "m(n),l=g.indexOf(\" \"),m=parseInt(g.substring(0,l),10);if(!is"
    "NaN(m))if(m<=h){a.push(n);continue}else if(m<f||f==0)f=m;c.p"
    "ush(g.substring(l+1,g.indexOf(\" \",l+1)))}}catch(q){}h=\"\";f&&"
    "(h=\"; expires=\"+(new Date(f)).toUTCString());\ndocument.cooki"
    "e=\"_GPSLSC=\"+c.join(\"!\")+\";path=/\"+h;d=0;for(k=a.length;d<k;"
    "++d)try{window.localStorage.removeItem(a[d])}catch(q){}b.g=!"
    "1}})};r.localStorageCacheInit=r.m;})();\n";

}  // namespace net_instaweb
