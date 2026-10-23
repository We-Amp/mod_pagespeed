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
// Automatically generated from critical_css_loader_opt.js

namespace net_instaweb {

const char* JS_critical_css_loader_opt =
    "(()=>{var n=function(e,r,i){if(e.addEventListener)e.addEvent"
    "Listener(r,i,!1);else if(e.attachEvent)e.attachEvent(\"on\"+r,"
    "i);else{var o=e[\"on\"+r];e[\"on\"+r]=function(){i.call(this),o&"
    "&o.call(this)}}};var s=Date.now||(function(){return+new Date"
    "});window.pagespeed=window.pagespeed||{};var t=window.pagesp"
    "eed;t.CriticalCssLoader=t.CriticalCssLoader||{};t.CriticalCs"
    "sLoader.DEFERRED_ATTRIBUTE_=\"data-pagespeed-deferred-css\";t."
    "CriticalCssLoader.started_=!1;t.CriticalCssLoader.apply_=fun"
    "ction(e){e.getAttribute(\"rel\")==\"preload\"&&e.setAttribute(\"r"
    "el\",\"stylesheet\")};t.CriticalCssLoader.onLinkEvent_=function"
    "(e){var r=e.target;r&&r.nodeName==\"LINK\"&&r.hasAttribute(t.C"
    "riticalCssLoader.DEFERRED_ATTRIBUTE_)&&t.CriticalCssLoader.a"
    "pply_(r)};t.CriticalCssLoader.applyAll=function(){for(var e="
    "document.querySelectorAll(\"link[\"+t.CriticalCssLoader.DEFERR"
    "ED_ATTRIBUTE_+\"]\"),r=0;r<e.length;++r)t.CriticalCssLoader.ap"
    "ply_(e[r])};t.CriticalCssLoader.Run=function(){if(!t.Critica"
    "lCssLoader.started_){t.CriticalCssLoader.started_=!0,documen"
    "t.addEventListener(\"load\",t.CriticalCssLoader.onLinkEvent_,!"
    "0),document.addEventListener(\"error\",t.CriticalCssLoader.onL"
    "inkEvent_,!0);var e=document.createElement(\"link\"),r=!!(e.re"
    "lList&&e.relList.supports&&e.relList.supports(\"preload\"));r|"
    "|n(document,\"DOMContentLoaded\",t.CriticalCssLoader.applyAll)"
    ",n(window,\"load\",t.CriticalCssLoader.applyAll),t.CriticalCss"
    "Loader.applyAll()}};})();\n";

}  // namespace net_instaweb
