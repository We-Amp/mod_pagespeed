(function(){var pagespeedutils = {MAX_POST_SIZE:131072, sendBeacon:function(a, b, d) {
  if (window.XMLHttpRequest) {
    var c = new XMLHttpRequest();
  } else if (window.ActiveXObject) {
    try {
      c = new ActiveXObject("Msxml2.XMLHTTP");
    } catch (f) {
      try {
        c = new ActiveXObject("Microsoft.XMLHTTP");
      } catch (g) {
      }
    }
  }
  if (!c) {
    return !1;
  }
  var e = a.indexOf("?") == -1 ? "?" : "&";
  a = a + e + "url=" + encodeURIComponent(b);
  c.open("POST", a);
  c.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
  c.send(d);
  return !0;
}, addHandler:function(a, b, d) {
  if (a.addEventListener) {
    a.addEventListener(b, d, !1);
  } else if (a.attachEvent) {
    a.attachEvent("on" + b, d);
  } else {
    var c = a["on" + b];
    a["on" + b] = function() {
      d.call(this);
      c && c.call(this);
    };
  }
}, getPosition:function(a) {
  for (var b = a.offsetTop, d = a.offsetLeft; a.offsetParent;) {
    a = a.offsetParent, b += a.offsetTop, d += a.offsetLeft;
  }
  return {top:b, left:d};
}, getWindowSize:function() {
  return {height:window.innerHeight || document.documentElement.clientHeight || document.body.clientHeight, width:window.innerWidth || document.documentElement.clientWidth || document.body.clientWidth};
}, inViewport:function(a, b) {
  a = pagespeedutils.getPosition(a);
  return pagespeedutils.positionInViewport(a, b);
}, positionInViewport:function(a, b) {
  return a.top < b.height && a.left < b.width;
}, getRequestAnimationFrame:function() {
  return window.requestAnimationFrame || window.webkitRequestAnimationFrame || window.mozRequestAnimationFrame || window.oRequestAnimationFrame || window.msRequestAnimationFrame || null;
}};
pagespeedutils.now = Date.now || function() {
  return +new Date();
};
window.pagespeed = window.pagespeed || {};
var pagespeed = window.pagespeed;
pagespeed.deferJsNs = {};
var deferJsNs = pagespeed.deferJsNs;
deferJsNs.DeferJs = function() {
  this.queue_ = [];
  this.logs = [];
  this.dynamicInsertedScriptCount_ = this.next_ = 0;
  this.dynamicInsertedScript_ = [];
  this.documentWriteHtml_ = "";
  this.eventListenersMap_ = {};
  this.jsMimeTypes = "application/ecmascript application/javascript application/x-ecmascript application/x-javascript text/ecmascript text/javascript text/javascript1.0 text/javascript1.1 text/javascript1.2 text/javascript1.3 text/javascript1.4 text/javascript1.5 text/jscript text/livescript text/x-ecmascript text/x-javascript".split(" ");
  this.overrideDefaultImplementation_ = !0;
  this.origGetElementById_ = document.getElementById;
  this.origGetElementsByTagName_ = document.getElementsByTagName;
  this.origDocWrite_ = document.write;
  this.origDocOpen_ = document.open;
  this.origDocClose_ = document.close;
  this.origDocAddEventListener_ = document.addEventListener;
  this.origWindowAddEventListener_ = window.addEventListener;
  this.origDocAttachEvent_ = document.attachEvent;
  this.origWindowAttachEvent_ = window.attachEvent;
  this.origCreateElement_ = document.createElement;
  this.state_ = deferJsNs.DeferJs.STATES.NOT_STARTED;
  this.eventState_ = deferJsNs.DeferJs.EVENT.NOT_STARTED;
  this.lastIncrementalRun_ = this.firstIncrementalRun_ = !0;
  this.incrementalScriptsDoneCallback_ = null;
  this.noDeferAsyncScriptsCount_ = 0;
  this.noDeferAsyncScripts_ = [];
  this.psaNotProcessed_ = this.psaScriptType_ = "";
  this.optLastIndex_ = -1;
};
deferJsNs.DeferJs.isExperimentalMode = !1;
deferJsNs.DeferJs.STATES = {NOT_STARTED:0, WAITING_FOR_NEXT_RUN:1, SCRIPTS_REGISTERED:2, SCRIPTS_EXECUTING:3, SYNC_SCRIPTS_DONE:4, WAITING_FOR_ONLOAD:5, SCRIPTS_DONE:6};
deferJsNs.DeferJs.EVENT = {NOT_STARTED:0, BEFORE_SCRIPTS:1, DOM_READY:2, LOAD:3, AFTER_SCRIPTS:4};
deferJsNs.DeferJs.PRIORITY_PSA_NOT_PROCESSED = "priority_psa_not_processed";
deferJsNs.DeferJs.PSA_NOT_PROCESSED = "psa_not_processed";
deferJsNs.DeferJs.PSA_CURRENT_NODE = "psa_current_node";
deferJsNs.DeferJs.PSA_TO_BE_DELETED = "psa_to_be_deleted";
deferJsNs.DeferJs.PSA_SCRIPT_TYPE = "text/psajs";
deferJsNs.DeferJs.PRIORITY_PSA_SCRIPT_TYPE = "text/prioritypsajs";
deferJsNs.DeferJs.PSA_ORIG_TYPE = "data-pagespeed-orig-type";
deferJsNs.DeferJs.PSA_ORIG_SRC = "data-pagespeed-orig-src";
deferJsNs.DeferJs.PSA_ORIG_INDEX = "data-pagespeed-orig-index";
deferJsNs.DeferJs.PAGESPEED_ONLOAD = "data-pagespeed-onload";
deferJsNs.DeferJs.prototype.log = function(a, b) {
  this.logs && (this.logs.push("" + a), b && (this.logs.push(b.message), typeof console != "undefined" && typeof console.log != "undefined" && console.log("PSA ERROR: " + a + b.message)));
};
deferJsNs.DeferJs.prototype.submitTask = function(a, b) {
  this.queue_.splice(b ? b : this.queue_.length, 0, a);
};
deferJsNs.DeferJs.prototype.globalEval = function(a, b) {
  b = this.cloneScriptNode(b);
  b.text = a;
  b.setAttribute("type", "text/javascript");
  a = this.getCurrentDomLocation();
  a.parentNode.insertBefore(b, a);
  return b;
};
deferJsNs.DeferJs.prototype.createIdVars = function() {
  for (var a = document.getElementsByTagName("*"), b = "", d = 0; d < a.length; d++) {
    if (a[d].hasAttribute("id")) {
      var c = a[d].getAttribute("id");
      if (c && c.search(/[-:.]/) == -1 && c.search(/^[0-9]/) == -1) {
        try {
          window[c] && window[c].tagName && (b += "var " + c + '=document.getElementById("' + c + '");');
        } catch (e) {
          this.log("Exception while evaluating.", e);
        }
      }
    }
  }
  b && (a = this.globalEval(b), a.setAttribute(deferJsNs.DeferJs.PSA_NOT_PROCESSED, ""), a.setAttribute(deferJsNs.DeferJs.PRIORITY_PSA_NOT_PROCESSED, ""));
};
deferJsNs.DeferJs.prototype.attemptPrefetchOrQueue = function(a) {
  var b = this.origCreateElement_.call(document, "link");
  b.setAttribute("rel", "preload");
  b.setAttribute("as", "script");
  b.setAttribute("href", a);
  document.head.appendChild(b);
};
deferJsNs.DeferJs.prototype.addNode = function(a, b, d) {
  var c = a.getAttribute(deferJsNs.DeferJs.PSA_ORIG_SRC) || a.getAttribute("src");
  c ? (d && this.attemptPrefetchOrQueue(c), this.addUrl(c, a, b)) : this.addStr(a.innerHTML || a.textContent || a.data || "", a, b);
};
deferJsNs.DeferJs.prototype.addStr = function(a, b, d) {
  if (this.isFireFox()) {
    this.addUrl("data:text/javascript," + encodeURIComponent(a), b, d);
  } else {
    this.logs.push("Add to queue str: " + a);
    var c = this;
    this.submitTask(function() {
      c.removeNotProcessedAttributeTillNode(b);
      c.nextPsaJsNode().setAttribute(deferJsNs.DeferJs.PSA_CURRENT_NODE, "");
      try {
        c.globalEval(a, b);
      } catch (e) {
        c.log("Exception while evaluating.", e);
      }
      c.log("Evaluated: " + a);
      c.runNext();
    }, d);
  }
};
deferJsNs.DeferJs.prototype.addStr = deferJsNs.DeferJs.prototype.addStr;
deferJsNs.DeferJs.prototype.cloneScriptNode = function(a) {
  var b = this.origCreateElement_.call(document, "script");
  if (a) {
    for (var d = a.attributes, c = d.length - 1; c >= 0; --c) {
      d[c].name != "type" && d[c].name != "src" && d[c].name != "async" && d[c].name != "defer" && d[c].name != deferJsNs.DeferJs.PSA_ORIG_TYPE && d[c].name != deferJsNs.DeferJs.PSA_ORIG_SRC && d[c].name != deferJsNs.DeferJs.PSA_ORIG_INDEX && d[c].name != deferJsNs.DeferJs.PSA_CURRENT_NODE && d[c].name != this.psaNotProcessed_ && (b.setAttribute(d[c].name, d[c].value), a.removeAttribute(d[c].name));
    }
  }
  return b;
};
deferJsNs.DeferJs.prototype.scriptOnLoad = function(a) {
  var b = this.origCreateElement_.call(document, "script");
  b.setAttribute("type", "text/javascript");
  b.async = !1;
  b.setAttribute(deferJsNs.DeferJs.PSA_TO_BE_DELETED, "");
  b.setAttribute(deferJsNs.DeferJs.PSA_NOT_PROCESSED, "");
  b.setAttribute(deferJsNs.DeferJs.PRIORITY_PSA_NOT_PROCESSED, "");
  var d = this, c = function() {
    if (document.querySelector) {
      var e = document.querySelector("[" + deferJsNs.DeferJs.PSA_TO_BE_DELETED + "]");
      e && e.parentNode.removeChild(e);
    }
    d.log("Executed: " + a);
    d.runNext();
  };
  deferJsNs.addOnload(b, c);
  pagespeedutils.addHandler(b, "error", c);
  b.src = "data:text/javascript," + encodeURIComponent("window.pagespeed.psatemp=0;");
  c = this.getCurrentDomLocation();
  c.parentNode.insertBefore(b, c);
};
deferJsNs.DeferJs.prototype.addUrl = function(a, b, d) {
  this.logs.push("Add to queue url: " + a);
  var c = this;
  this.submitTask(function() {
    c.removeNotProcessedAttributeTillNode(b);
    var e = c.cloneScriptNode(b);
    e.setAttribute("type", "text/javascript");
    var f = !0;
    "async" in e ? e.async = !1 : e.readyState && (f = !1, pagespeedutils.addHandler(e, "readystatechange", function() {
      if (e.readyState == "complete" || e.readyState == "loaded") {
        e.onreadystatechange = null, c.log("Executed: " + a), c.runNext();
      }
    }));
    e.setAttribute("src", a);
    var g = b.innerHTML || b.textContent || b.data;
    g && e.appendChild(document.createTextNode(g));
    g = c.nextPsaJsNode();
    g.setAttribute(deferJsNs.DeferJs.PSA_CURRENT_NODE, "");
    g.parentNode.insertBefore(e, g);
    f && c.scriptOnLoad(a);
  }, d);
};
deferJsNs.DeferJs.prototype.addUrl = deferJsNs.DeferJs.prototype.addUrl;
deferJsNs.DeferJs.prototype.removeNotProcessedAttributeTillNode = function(a) {
  if (document.querySelectorAll && !(this.getIEVersion() <= 8)) {
    for (var b = document.querySelectorAll("[" + this.psaNotProcessed_ + "]"), d = 0; d < b.length; d++) {
      var c = b.item(d);
      if (c == a) {
        break;
      }
      c.getAttribute("type") != this.psaScriptType_ && c.removeAttribute(this.psaNotProcessed_);
    }
  }
};
deferJsNs.DeferJs.prototype.setNotProcessedAttributeForNodes = function() {
  for (var a = this.origGetElementsByTagName_.call(document, "*"), b = 0; b < a.length; b++) {
    a.item(b).setAttribute(this.psaNotProcessed_, "");
  }
};
deferJsNs.DeferJs.prototype.nextPsaJsNode = function() {
  var a = null;
  document.querySelector && (a = document.querySelector('[type="' + this.psaScriptType_ + '"]'));
  return a;
};
deferJsNs.DeferJs.prototype.getCurrentDomLocation = function() {
  var a;
  document.querySelector && (a = document.querySelector("[" + deferJsNs.DeferJs.PSA_CURRENT_NODE + "]"));
  return a || this.origGetElementsByTagName_.call(document, "psanode")[0];
};
deferJsNs.DeferJs.prototype.removeCurrentDomLocation = function() {
  var a = this.getCurrentDomLocation();
  a.nodeName == "SCRIPT" && a.parentNode.removeChild(a);
};
deferJsNs.DeferJs.prototype.onComplete = function() {
  if (!(this.state_ >= deferJsNs.DeferJs.STATES.WAITING_FOR_ONLOAD)) {
    if (this.lastIncrementalRun_ && this.isLowPriorityDeferJs() && (this.getIEVersion() && document.documentElement.originalDoScroll && (document.documentElement.doScroll = document.documentElement.originalDoScroll), Object.defineProperty && delete document.readyState, this.getIEVersion() && Object.defineProperty && delete document.all), this.overrideDefaultImplementation_ = !1, this.lastIncrementalRun_) {
      if (this.state_ = deferJsNs.DeferJs.STATES.WAITING_FOR_ONLOAD, this.isLowPriorityDeferJs()) {
        var a = this;
        document.readyState != "complete" ? deferJsNs.addOnload(window, function() {
          a.fireOnload();
        }) : (document.onreadystatechange && this.exec(document.onreadystatechange, document), window.onload && (psaAddEventListener(window, "onload", window.onload), window.onload = null), this.fireOnload());
      } else {
        this.highPriorityFinalize();
      }
    } else {
      this.state_ = deferJsNs.DeferJs.STATES.WAITING_FOR_NEXT_RUN, this.firstIncrementalRun_ = !1, this.incrementalScriptsDoneCallback_ && this.isLowPriorityDeferJs() ? (pagespeed.deferJs = pagespeed.highPriorityDeferJs, pagespeed.deferJs = pagespeed.highPriorityDeferJs, this.exec(this.incrementalScriptsDoneCallback_), this.incrementalScriptsDoneCallback_ = null) : this.highPriorityFinalize();
    }
  }
};
deferJsNs.DeferJs.prototype.fireOnload = function() {
  this.isLowPriorityDeferJs() && (this.addDeferredOnloadListeners(), pagespeed.highPriorityDeferJs.fireOnload());
  this.fireEvent(deferJsNs.DeferJs.EVENT.LOAD);
  if (this.isLowPriorityDeferJs()) {
    for (var a = document.body.getElementsByTagName("psanode"), b = a.length - 1; b >= 0; b--) {
      document.body.removeChild(a[b]);
    }
    a = document.body.getElementsByClassName("psa_prefetch_container");
    for (b = a.length - 1; b >= 0; b--) {
      a[b].parentNode.removeChild(a[b]);
    }
  }
  this.state_ = deferJsNs.DeferJs.STATES.SCRIPTS_DONE;
  this.fireEvent(deferJsNs.DeferJs.EVENT.AFTER_SCRIPTS);
};
deferJsNs.DeferJs.prototype.highPriorityFinalize = function() {
  var a = this;
  window.setTimeout(function() {
    pagespeed.deferJs = pagespeed.lowPriorityDeferJs;
    pagespeed.deferJs = pagespeed.lowPriorityDeferJs;
    a.incrementalScriptsDoneCallback_ ? (pagespeed.deferJs.registerScriptTags(a.incrementalScriptsDoneCallback_, a.optLastIndex_), a.incrementalScriptsDoneCallback_ = null) : pagespeed.deferJs.registerScriptTags();
    pagespeed.deferJs.execute();
  }, 0);
};
deferJsNs.DeferJs.prototype.checkNodeInDom = function(a) {
  for (; a = a.parentNode;) {
    if (a == document) {
      return !0;
    }
  }
  return !1;
};
deferJsNs.DeferJs.prototype.getNumScriptsWithNoOnload = function(a) {
  for (var b = 0, d = a.length, c = 0; c < d; ++c) {
    var e = a[c], f = e.parentNode, g = e.src, h = e.textContent;
    (this.getIEVersion() > 8 && (!f || g == "" && h == "") || !(this.getIEVersion() || this.checkNodeInDom(e) && g != "")) && b++;
  }
  return b;
};
deferJsNs.DeferJs.prototype.canCallOnComplete = function() {
  if (this.state_ != deferJsNs.DeferJs.STATES.SYNC_SCRIPTS_DONE) {
    return !1;
  }
  var a = 0;
  this.dynamicInsertedScriptCount_ != 0 && (a = this.getNumScriptsWithNoOnload(this.dynamicInsertedScript_));
  return this.dynamicInsertedScriptCount_ == a ? !0 : !1;
};
deferJsNs.DeferJs.prototype.scriptsAreDone = function() {
  return this.state_ === deferJsNs.DeferJs.STATES.SCRIPTS_DONE;
};
deferJsNs.DeferJs.prototype.scriptsAreDone = deferJsNs.DeferJs.prototype.scriptsAreDone;
deferJsNs.DeferJs.prototype.runNext = function() {
  this.handlePendingDocumentWrites();
  this.removeCurrentDomLocation();
  if (this.next_ < this.queue_.length) {
    this.next_++, this.queue_[this.next_ - 1].call(window);
  } else {
    if (this.lastIncrementalRun_) {
      if (this.state_ = deferJsNs.DeferJs.STATES.SYNC_SCRIPTS_DONE, this.removeNotProcessedAttributeTillNode(), this.fireEvent(deferJsNs.DeferJs.EVENT.DOM_READY), this.canCallOnComplete()) {
        this.onComplete();
      }
    } else {
      this.onComplete();
    }
  }
};
deferJsNs.DeferJs.prototype.nodeListToArray = function(a) {
  for (var b = [], d = a.length, c = 0; c < d; ++c) {
    b.push(a.item(c));
  }
  return b;
};
deferJsNs.DeferJs.prototype.setUp = function() {
  var a = this;
  if (this.firstIncrementalRun_ && !this.isLowPriorityDeferJs()) {
    var b = document.createElement("psanode");
    b.setAttribute("psa_dw_target", "true");
    document.body.appendChild(b);
    this.getIEVersion() && this.createIdVars();
    if (Object.defineProperty) {
      try {
        var d = {configurable:!0, get:function() {
          return a.state_ >= deferJsNs.DeferJs.STATES.SYNC_SCRIPTS_DONE ? "interactive" : "loading";
        }};
        Object.defineProperty(document, "readyState", d);
      } catch (c) {
        this.log("Exception while overriding document.readyState.", c);
      }
    }
    if (this.getIEVersion() && (document.documentElement.originalDoScroll = document.documentElement.doScroll, document.documentElement.doScroll = function() {
      throw "psa exception";
    }, Object.defineProperty)) {
      try {
        d = {configurable:!0, get:function() {
        }}, Object.defineProperty(document, "all", d);
      } catch (c) {
        this.log("Exception while overriding document.all.", c);
      }
    }
  }
  this.overrideAddEventListeners();
  document.writeln = function(c) {
    a.writeHtml(c + "\n");
  };
  document.write = function(c) {
    a.writeHtml(c);
  };
  document.open = function() {
    a.overrideDefaultImplementation_ || a.origDocOpen_.call(document);
  };
  document.close = function() {
    a.overrideDefaultImplementation_ || a.origDocClose_.call(document);
  };
  document.getElementById = function(c) {
    a.handlePendingDocumentWrites();
    c = a.origGetElementById_.call(document, c);
    return c == null || c.hasAttribute(a.psaNotProcessed_) ? null : c;
  };
  !document.querySelectorAll || a.getIEVersion() <= 8 || (document.getElementsByTagName = function(c) {
    if (a.overrideDefaultImplementation_) {
      try {
        return document.querySelectorAll(c + ":not([" + a.psaNotProcessed_ + "])");
      } catch (e) {
      }
    }
    return a.origGetElementsByTagName_.call(document, c);
  });
  document.createElement = function(c) {
    var e = a.origCreateElement_.call(document, c);
    a.overrideDefaultImplementation_ && c.toLowerCase() == "script" && (a.dynamicInsertedScript_.push(e), a.dynamicInsertedScriptCount_++, c = function() {
      a.dynamicInsertedScriptCount_--;
      var f = a.dynamicInsertedScript_.indexOf(this);
      if (f != -1 && (a.dynamicInsertedScript_.splice(f, 1), a.canCallOnComplete())) {
        a.onComplete();
      }
    }, deferJsNs.addOnload(e, c), pagespeedutils.addHandler(e, "error", c));
    return e;
  };
};
deferJsNs.DeferJs.prototype.execute = function() {
  if (this.state_ == deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED) {
    var a = 0;
    this.noDeferAsyncScriptsCount_ != 0 && (a = this.getNumScriptsWithNoOnload(this.noDeferAsyncScripts_));
    this.noDeferAsyncScriptsCount_ == a && this.run();
  }
};
deferJsNs.DeferJs.prototype.execute = deferJsNs.DeferJs.prototype.execute;
deferJsNs.DeferJs.prototype.run = function() {
  this.state_ == deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED && (this.firstIncrementalRun_ && this.fireEvent(deferJsNs.DeferJs.EVENT.BEFORE_SCRIPTS), this.state_ = deferJsNs.DeferJs.STATES.SCRIPTS_EXECUTING, this.setUp(), this.runNext());
};
deferJsNs.DeferJs.prototype.run = deferJsNs.DeferJs.prototype.run;
deferJsNs.DeferJs.prototype.parseHtml = function(a) {
  var b = this.origCreateElement_.call(document, "div");
  b.innerHTML = "<div>_</div>" + a;
  b.removeChild(b.firstChild);
  return b;
};
deferJsNs.DeferJs.prototype.disown = function(a) {
  var b = a.parentNode;
  b && b.removeChild(a);
};
deferJsNs.DeferJs.prototype.insertNodesBeforeElem = function(a, b) {
  a = this.nodeListToArray(a);
  for (var d = a.length, c = b.parentNode, e = 0; e < d; ++e) {
    var f = a[e];
    this.disown(f);
    c.insertBefore(f, b);
  }
};
deferJsNs.DeferJs.prototype.isJSNode = function(a) {
  return a.nodeName != "SCRIPT" ? !1 : a.hasAttribute("type") ? (a = a.getAttribute("type"), !a || this.jsMimeTypes.indexOf(a) != -1) : a.hasAttribute("language") ? (a = a.getAttribute("language"), !a || this.jsMimeTypes.indexOf("text/" + a.toLowerCase()) != -1) : !0;
};
deferJsNs.DeferJs.prototype.markNodesAndExtractScriptNodes = function(a, b) {
  if (a.childNodes) {
    a = this.nodeListToArray(a.childNodes);
    for (var d = a.length, c = 0; c < d; ++c) {
      var e = a[c];
      e.nodeName == "SCRIPT" ? this.isJSNode(e) && (b.push(e), e.setAttribute(deferJsNs.DeferJs.PSA_ORIG_TYPE, e.type), e.setAttribute("type", this.psaScriptType_), e.setAttribute(deferJsNs.DeferJs.PSA_ORIG_SRC, e.src), e.setAttribute("src", ""), e.setAttribute(this.psaNotProcessed_, "")) : this.markNodesAndExtractScriptNodes(e, b);
    }
  }
};
deferJsNs.DeferJs.prototype.deferScripts = function(a, b) {
  for (var d = a.length, c = 0; c < d; ++c) {
    this.addNode(a[c], b + c, !!c);
  }
};
deferJsNs.DeferJs.prototype.insertHtml = function(a, b, d) {
  a = this.parseHtml(a);
  var c = [];
  this.markNodesAndExtractScriptNodes(a, c);
  d ? this.insertNodesBeforeElem(a.childNodes, d) : this.log("Unable to insert nodes, no context element found");
  this.deferScripts(c, b);
};
deferJsNs.DeferJs.prototype.handlePendingDocumentWrites = function() {
  if (this.documentWriteHtml_ != "") {
    this.log("handle_dw: " + this.documentWriteHtml_);
    var a = this.documentWriteHtml_;
    this.documentWriteHtml_ = "";
    var b = this.getCurrentDomLocation();
    this.insertHtml(a, this.next_, b);
  }
};
deferJsNs.DeferJs.prototype.writeHtml = function(a) {
  this.overrideDefaultImplementation_ ? (this.log("dw: " + a), this.documentWriteHtml_ += a) : this.origDocWrite_.call(document, a);
};
deferJsNs.DeferJs.prototype.addDeferredOnloadListeners = function() {
  var a;
  document.querySelectorAll && (a = document.querySelectorAll("[" + deferJsNs.DeferJs.PAGESPEED_ONLOAD + "][data-pagespeed-loaded]"));
  for (var b = 0; b < a.length; b++) {
    var d = a.item(b), c = "var psaFunc=function() {" + d.getAttribute(deferJsNs.DeferJs.PAGESPEED_ONLOAD) + "};";
    window.eval.call(window, c);
    typeof window.psaFunc != "function" ? this.log("Function is not defined", Error("")) : psaAddEventListener(d, "onload", window.psaFunc);
  }
};
deferJsNs.DeferJs.prototype.addBeforeDeferRunFunctions = function(a) {
  psaAddEventListener(window, "onbeforescripts", a);
};
deferJsNs.DeferJs.prototype.addBeforeDeferRunFunctions = deferJsNs.DeferJs.prototype.addBeforeDeferRunFunctions;
deferJsNs.DeferJs.prototype.addAfterDeferRunFunctions = function(a) {
  psaAddEventListener(window, "onafterscripts", a);
};
deferJsNs.DeferJs.prototype.addAfterDeferRunFunctions = deferJsNs.DeferJs.prototype.addAfterDeferRunFunctions;
deferJsNs.DeferJs.prototype.fireEvent = function(a) {
  this.eventState_ = a;
  this.log("Firing Event: " + a);
  a = this.eventListenersMap_[a] || [];
  for (var b = 0; b < a.length; ++b) {
    this.exec(a[b]);
  }
  a.length = 0;
};
deferJsNs.DeferJs.prototype.exec = function(a, b) {
  try {
    a.call(b || window);
  } catch (d) {
    this.log("Exception while evaluating.", d);
  }
};
deferJsNs.DeferJs.prototype.overrideAddEventListeners = function() {
  var a = this;
  window.addEventListener ? (document.addEventListener = function(b, d, c) {
    psaAddEventListener(document, b, d, a.origDocAddEventListener_, c);
  }, window.addEventListener = function(b, d, c) {
    psaAddEventListener(window, b, d, a.origWindowAddEventListener_, c);
  }) : window.attachEvent && (document.attachEvent = function(b, d) {
    psaAddEventListener(document, b, d, a.origDocAttachEvent_);
  }, window.attachEvent = function(b, d) {
    psaAddEventListener(window, b, d, a.origWindowAttachEvent_);
  });
};
var psaAddEventListener = function(a, b, d, c, e) {
  var f = pagespeed.deferJs;
  if (f.state_ >= deferJsNs.DeferJs.STATES.WAITING_FOR_ONLOAD) {
    if (c) {
      c.call(a, b, d, e);
      return;
    }
    if (f.state_ >= deferJsNs.DeferJs.STATES.SCRIPTS_DONE) {
      return;
    }
  }
  if (f.eventState_ < deferJsNs.DeferJs.EVENT.DOM_READY && (b == "DOMContentLoaded" || b == "readystatechange" || b == "onDOMContentLoaded" || b == "onreadystatechange")) {
    b = deferJsNs.DeferJs.EVENT.DOM_READY;
    var g = "DOMContentLoaded";
  } else if (f.eventState_ < deferJsNs.DeferJs.EVENT.LOAD && (b == "load" || b == "onload")) {
    b = deferJsNs.DeferJs.EVENT.LOAD, g = "load";
  } else if (b == "onbeforescripts") {
    b = deferJsNs.DeferJs.EVENT.BEFORE_SCRIPTS;
  } else if (b == "onafterscripts") {
    b = deferJsNs.DeferJs.EVENT.AFTER_SCRIPTS;
  } else {
    c && c.call(a, b, d, e);
    return;
  }
  f.eventListenersMap_[b] || (f.eventListenersMap_[b] = []);
  f.eventListenersMap_[b].push(function() {
    var h = {bubbles:!1, cancelable:!1, eventPhase:2};
    h.timeStamp = (new Date()).getTime();
    h.type = g;
    h.target = a != window ? a : document;
    h.currentTarget = a;
    d.call(a, h);
  });
};
deferJsNs.DeferJs.prototype.registerScriptTags = function(a, b) {
  if (!(this.state_ >= deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED)) {
    if (a) {
      if (!deferJsNs.DeferJs.isExperimentalMode) {
        a();
        return;
      }
      this.lastIncrementalRun_ = !1;
      this.incrementalScriptsDoneCallback_ = a;
      b && (this.optLastIndex_ = b);
    } else {
      this.lastIncrementalRun_ = !0;
    }
    this.state_ = deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED;
    b = document.getElementsByTagName("script");
    for (var d = b.length, c = 0; c < d; ++c) {
      var e = this.queue_.length == this.next_, f = b[c];
      f.getAttribute("type") == this.psaScriptType_ && (a ? f.getAttribute(deferJsNs.DeferJs.PSA_ORIG_INDEX) <= this.optLastIndex_ && this.addNode(f, void 0, !e) : (f.getAttribute(deferJsNs.DeferJs.PSA_ORIG_INDEX) < this.optLastIndex_ && this.log("Executing a script twice. Orig_Index: " + f.getAttribute(deferJsNs.DeferJs.PSA_ORIG_INDEX), Error("")), this.addNode(f, void 0, !e)));
    }
  }
};
deferJsNs.DeferJs.prototype.registerScriptTags = deferJsNs.DeferJs.prototype.registerScriptTags;
deferJsNs.addOnload = function(a, b) {
  pagespeedutils.addHandler(a, "load", b);
};
pagespeed.addOnload = deferJsNs.addOnload;
deferJsNs.DeferJs.prototype.isFireFox = function() {
  return navigator.userAgent.indexOf("Firefox") != -1;
};
deferJsNs.DeferJs.prototype.isWebKit = function() {
  return navigator.userAgent.indexOf("AppleWebKit") != -1;
};
deferJsNs.DeferJs.prototype.getIEVersion = function() {
  var a = /(?:MSIE.(\d+\.\d+))/.exec(navigator.userAgent);
  return a && a[1] ? document.documentMode || parseFloat(a[1]) : NaN;
};
deferJsNs.DeferJs.prototype.setType = function(a) {
  this.psaScriptType_ = a;
};
deferJsNs.DeferJs.prototype.setPsaNotProcessed = function(a) {
  this.psaNotProcessed_ = a;
};
deferJsNs.DeferJs.prototype.isLowPriorityDeferJs = function() {
  return this.psaScriptType_ == deferJsNs.DeferJs.PSA_SCRIPT_TYPE ? !0 : !1;
};
deferJsNs.DeferJs.prototype.noDeferCreateElementOverride = function() {
  var a = this;
  document.createElement = function(b) {
    var d = a.origCreateElement_.call(document, b);
    a.overrideDefaultImplementation_ && b.toLowerCase() == "script" && (a.noDeferAsyncScripts_.push(d), a.noDeferAsyncScriptsCount_++, b = function() {
      var c = a.noDeferAsyncScripts_.indexOf(this);
      c != -1 && (a.noDeferAsyncScripts_.splice(c, 1), a.noDeferAsyncScriptsCount_--, a.execute());
    }, deferJsNs.addOnload(d, b), pagespeedutils.addHandler(d, "error", b));
    return d;
  };
};
deferJsNs.DeferJs.prototype.isExperimentalMode = function() {
  return deferJsNs.DeferJs.isExperimentalMode;
};
deferJsNs.DeferJs.prototype.isExperimentalMode = deferJsNs.DeferJs.prototype.isExperimentalMode;
deferJsNs.deferInit = function() {
  pagespeed.deferJs || (deferJsNs.DeferJs.isExperimentalMode = pagespeed.defer_js_experimental, pagespeed.highPriorityDeferJs = new deferJsNs.DeferJs(), pagespeed.highPriorityDeferJs.setType(deferJsNs.DeferJs.PRIORITY_PSA_SCRIPT_TYPE), pagespeed.highPriorityDeferJs.setPsaNotProcessed(deferJsNs.DeferJs.PRIORITY_PSA_NOT_PROCESSED), pagespeed.highPriorityDeferJs.setNotProcessedAttributeForNodes(), pagespeed.lowPriorityDeferJs = new deferJsNs.DeferJs(), pagespeed.lowPriorityDeferJs.setType(deferJsNs.DeferJs.PSA_SCRIPT_TYPE), 
  pagespeed.lowPriorityDeferJs.setPsaNotProcessed(deferJsNs.DeferJs.PSA_NOT_PROCESSED), pagespeed.lowPriorityDeferJs.setNotProcessedAttributeForNodes(), pagespeed.deferJs = pagespeed.highPriorityDeferJs, pagespeed.deferJs.noDeferCreateElementOverride(), pagespeed.deferJs = pagespeed.deferJs);
};
deferJsNs.deferInit();
pagespeed.deferJsStarted = !1;
deferJsNs.startDeferJs = function() {
  pagespeed.deferJsStarted || (pagespeed.deferJsStarted = !0, pagespeed.deferJs.registerScriptTags(), pagespeed.deferJs.execute());
};
deferJsNs.startDeferJs = deferJsNs.startDeferJs;
pagespeedutils.addHandler(document, "DOMContentLoaded", deferJsNs.startDeferJs);
deferJsNs.addOnload(window, deferJsNs.startDeferJs);
})();
