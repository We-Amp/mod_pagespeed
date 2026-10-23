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
// Automatically generated from delay_images_inline_opt.js

namespace net_instaweb {

const char* JS_delay_images_inline_opt =
    "(()=>{window.pagespeed=window.pagespeed||{};var a=window.pag"
    "espeed;a.DelayImagesInline=function(){this.inlineMap_={}};a."
    "DelayImagesInline.prototype.addLowResImages=function(e,n){th"
    "is.inlineMap_[e]=n};a.DelayImagesInline.prototype.replaceEle"
    "mentSrc=function(e){for(var n=0;n<e.length;++n){var t=e[n].g"
    "etAttribute(\"data-pagespeed-high-res-src\"),p=e[n].getAttribu"
    "te(\"src\");if(t&&!p){var i=this.inlineMap_[t];i&&e[n].setAttr"
    "ibute(\"src\",i)}}};a.DelayImagesInline.prototype.replaceWithL"
    "owRes=function(){this.replaceElementSrc(document.getElements"
    "ByTagName(\"img\")),this.replaceElementSrc(document.getElement"
    "sByTagName(\"input\"))};a.delayImagesInlineInit=function(){var"
    " e=new a.DelayImagesInline;a.delayImagesInline=e};})();\n";

}  // namespace net_instaweb
