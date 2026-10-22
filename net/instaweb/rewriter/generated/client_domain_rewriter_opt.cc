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
// Automatically generated from client_domain_rewriter_opt.js

namespace net_instaweb {

const char* JS_client_domain_rewriter_opt =
    "(()=>{var t=13;window.pagespeed=window.pagespeed||{};var i=w"
    "indow.pagespeed;i.ClientDomainRewriter=function(e){this.mapp"
    "edDomainNames_=e};i.ClientDomainRewriter.prototype.anchorLis"
    "tener=function(e){if(e=e||window.event,!(e.type==\"keypress\"&"
    "&e.keyCode!=t)){for(var n=e.target;n!=null;n=n.parentNode)if"
    "(n.tagName==\"A\"){this.processEvent(n.href,e);return}}};i.Cli"
    "entDomainRewriter.prototype.addEventListeners=function(){var"
    " e=this;document.body.onclick=function(n){e.anchorListener(n"
    ")},document.body.onkeypress=function(n){e.anchorListener(n)}"
    "};i.ClientDomainRewriter.prototype.processEvent=function(e,n"
    "){for(var o=0;o<this.mappedDomainNames_.length;o++)if(e.inde"
    "xOf(this.mappedDomainNames_[o])==0){window.location=window.l"
    "ocation.protocol+\"//\"+window.location.hostname+\"/\"+e.substr("
    "this.mappedDomainNames_[o].length),n.preventDefault();return"
    "}};i.clientDomainRewriterInit=function(e){var n=new i.Client"
    "DomainRewriter(e);i.clientDomainRewriter=n,n.addEventListene"
    "rs()};})();\n";

}  // namespace net_instaweb
