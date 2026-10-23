(() => {
  var addHandler = function(elem, eventName, func) {
    if (elem.addEventListener) {
      elem.addEventListener(eventName, func, false);
    } else if (elem.attachEvent) {
      elem.attachEvent("on" + eventName, func);
    } else {
      var oldHandler = elem["on" + eventName];
      elem["on" + eventName] = function() {
        func.call(this);
        if (oldHandler) {
          oldHandler.call(this);
        }
      };
    }
  };
  var now = Date.now || (function() {
    return +new Date();
  });

  window["pagespeed"] = window["pagespeed"] || {};
  var pagespeed = window["pagespeed"];
  pagespeed["deferJsNs"] = {};
  var deferJsNs = pagespeed["deferJsNs"];
  deferJsNs.DeferJs = function() {
    this.queue_ = [];
    this.logs = [];
    this.next_ = 0;
    this.dynamicInsertedScriptCount_ = 0;
    this.dynamicInsertedScript_ = [];
    this.documentWriteHtml_ = "";
    this.eventListenersMap_ = {};
    this.jsMimeTypes = [
      "application/ecmascript",
      "application/javascript",
      "application/x-ecmascript",
      "application/x-javascript",
      "text/ecmascript",
      "text/javascript",
      "text/javascript1.0",
      "text/javascript1.1",
      "text/javascript1.2",
      "text/javascript1.3",
      "text/javascript1.4",
      "text/javascript1.5",
      "text/jscript",
      "text/livescript",
      "text/x-ecmascript",
      "text/x-javascript"
    ];
    this.overrideDefaultImplementation_ = true;
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
    this.firstIncrementalRun_ = true;
    this.lastIncrementalRun_ = true;
    this.incrementalScriptsDoneCallback_ = null;
    this.noDeferAsyncScriptsCount_ = 0;
    this.noDeferAsyncScripts_ = [];
    this.psaScriptType_ = "";
    this.psaNotProcessed_ = "";
    this.optLastIndex_ = -1;
  };
  deferJsNs.DeferJs.isExperimentalMode = false;
  deferJsNs.DeferJs.STATES = {
    NOT_STARTED: 0,
    WAITING_FOR_NEXT_RUN: 1,
    SCRIPTS_REGISTERED: 2,
    SCRIPTS_EXECUTING: 3,
    SYNC_SCRIPTS_DONE: 4,
    WAITING_FOR_ONLOAD: 5,
    SCRIPTS_DONE: 6
  };
  deferJsNs.DeferJs.EVENT = {
    NOT_STARTED: 0,
    BEFORE_SCRIPTS: 1,
    DOM_READY: 2,
    LOAD: 3,
    AFTER_SCRIPTS: 4
  };
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
  deferJsNs.DeferJs.prototype.log = function(line, opt_exception) {
    if (this.logs) {
      this.logs.push("" + line);
      if (opt_exception) {
        this.logs.push(opt_exception.message);
        if (typeof console != "undefined" && typeof console.log != "undefined") {
          console.log("PSA ERROR: " + line + opt_exception.message);
        }
      }
    }
  };
  deferJsNs.DeferJs.prototype.submitTask = function(task, opt_pos) {
    var pos = opt_pos ? opt_pos : this.queue_.length;
    this.queue_.splice(pos, 0, task);
  };
  deferJsNs.DeferJs.prototype.globalEval = function(str, opt_script_elem) {
    var script = this.cloneScriptNode(opt_script_elem);
    script.text = str;
    script.setAttribute("type", "text/javascript");
    var currentElem = this.getCurrentDomLocation();
    currentElem.parentNode.insertBefore(script, currentElem);
    return script;
  };
  deferJsNs.DeferJs.prototype.createIdVars = function() {
    var elems = document.getElementsByTagName("*");
    var idVarsString = "";
    for (var i = 0; i < elems.length; i++) {
      if (elems[i].hasAttribute("id")) {
        var idStr = elems[i].getAttribute("id");
        if (idStr && idStr.search(/[-:.]/) == -1 && idStr.search(/^[0-9]/) == -1) {
          try {
            if (window[idStr] && window[idStr].tagName) {
              idVarsString += "var " + idStr + '=document.getElementById("' + idStr + '");';
            }
          } catch (err) {
            this.log("Exception while evaluating.", err);
          }
        }
      }
    }
    if (idVarsString) {
      var script = this.globalEval(idVarsString);
      script.setAttribute(deferJsNs.DeferJs.PSA_NOT_PROCESSED, "");
      script.setAttribute(deferJsNs.DeferJs.PRIORITY_PSA_NOT_PROCESSED, "");
    }
  };
  deferJsNs.DeferJs.prototype.attemptPrefetchOrQueue = function(url) {
    var newLink = this.origCreateElement_.call(document, "link");
    newLink.setAttribute("rel", "preload");
    newLink.setAttribute("as", "script");
    newLink.setAttribute("href", url);
    document.head.appendChild(newLink);
  };
  deferJsNs.DeferJs.prototype.addNode = function(script, opt_pos, opt_prefetch) {
    var src = script.getAttribute(deferJsNs.DeferJs.PSA_ORIG_SRC) || script.getAttribute("src");
    if (src) {
      if (opt_prefetch) {
        this.attemptPrefetchOrQueue(src);
      }
      this.addUrl(src, script, opt_pos);
    } else {
      var str = script.innerHTML || script.textContent || script.data || "";
      this.addStr(str, script, opt_pos);
    }
  };
  deferJsNs.DeferJs.prototype.addStr = function(str, script_elem, opt_pos) {
    this.logs.push("Add to queue str: " + str);
    var me = this;
    this.submitTask(function() {
      me.removeNotProcessedAttributeTillNode(script_elem);
      var node = me.nextPsaJsNode();
      node.setAttribute(deferJsNs.DeferJs.PSA_CURRENT_NODE, "");
      try {
        me.globalEval(str, script_elem);
      } catch (err) {
        me.log("Exception while evaluating.", err);
      }
      me.log("Evaluated: " + str);
      me.runNext();
    }, opt_pos);
  };
  deferJsNs.DeferJs.prototype["addStr"] = deferJsNs.DeferJs.prototype.addStr;
  deferJsNs.DeferJs.prototype.cloneScriptNode = function(opt_script_elem) {
    var newScript = this.origCreateElement_.call(document, "script");
    if (opt_script_elem) {
      for (var a = opt_script_elem.attributes, n = a.length, i = n - 1; i >= 0; --i) {
        if (a[i].name != "type" && a[i].name != "src" && a[i].name != "async" && a[i].name != "defer" && a[i].name != deferJsNs.DeferJs.PSA_ORIG_TYPE && a[i].name != deferJsNs.DeferJs.PSA_ORIG_SRC && a[i].name != deferJsNs.DeferJs.PSA_ORIG_INDEX && a[i].name != deferJsNs.DeferJs.PSA_CURRENT_NODE && a[i].name != this.psaNotProcessed_) {
          newScript.setAttribute(a[i].name, a[i].value);
          opt_script_elem.removeAttribute(a[i].name);
        }
      }
    }
    return newScript;
  };
  deferJsNs.DeferJs.prototype.scriptOnLoad = function(url) {
    var script = this.origCreateElement_.call(document, "script");
    script.setAttribute("type", "text/javascript");
    script.async = false;
    script.setAttribute(deferJsNs.DeferJs.PSA_TO_BE_DELETED, "");
    script.setAttribute(deferJsNs.DeferJs.PSA_NOT_PROCESSED, "");
    script.setAttribute(deferJsNs.DeferJs.PRIORITY_PSA_NOT_PROCESSED, "");
    var me = this;
    var runNextHandler = function() {
      if (document.querySelector) {
        var node = document.querySelector(
          "[" + deferJsNs.DeferJs.PSA_TO_BE_DELETED + "]"
        );
        if (node) {
          node.parentNode.removeChild(node);
        }
      }
      me.log("Executed: " + url);
      me.runNext();
    };
    deferJsNs.addOnload(script, runNextHandler);
    addHandler(script, "error", runNextHandler);
    script.src = "data:text/javascript," + encodeURIComponent("window.pagespeed.psatemp=0;");
    var currentElem = this.getCurrentDomLocation();
    currentElem.parentNode.insertBefore(script, currentElem);
  };
  deferJsNs.DeferJs.prototype.addUrl = function(url, script_elem, opt_pos) {
    this.logs.push("Add to queue url: " + url);
    var me = this;
    this.submitTask(function() {
      me.removeNotProcessedAttributeTillNode(script_elem);
      var script = me.cloneScriptNode(script_elem);
      script.setAttribute("type", "text/javascript");
      var useSyncScript = true;
      if ("async" in script) {
        script.async = false;
      } else if (script.readyState) {
        useSyncScript = false;
        var stateChangeHandler = function() {
          if (script.readyState == "complete" || script.readyState == "loaded") {
            script.onreadystatechange = null;
            me.log("Executed: " + url);
            me.runNext();
          }
        };
        addHandler(script, "readystatechange", stateChangeHandler);
      }
      script.setAttribute("src", url);
      var str = script_elem.innerHTML || script_elem.textContent || script_elem.data;
      if (str) {
        script.appendChild(document.createTextNode(str));
      }
      var currentElem = me.nextPsaJsNode();
      currentElem.setAttribute(deferJsNs.DeferJs.PSA_CURRENT_NODE, "");
      currentElem.parentNode.insertBefore(script, currentElem);
      if (useSyncScript) {
        me.scriptOnLoad(url);
      }
    }, opt_pos);
  };
  deferJsNs.DeferJs.prototype["addUrl"] = deferJsNs.DeferJs.prototype.addUrl;
  deferJsNs.DeferJs.prototype.removeNotProcessedAttributeTillNode = function(opt_node) {
    if (document.querySelectorAll && !(this.getIEVersion() <= 8)) {
      var nodes = document.querySelectorAll(
        "[" + this.psaNotProcessed_ + "]"
      );
      for (var i = 0; i < nodes.length; i++) {
        var dom_node = nodes.item(i);
        if (dom_node == opt_node) {
          return;
        }
        if (dom_node.getAttribute("type") != this.psaScriptType_) {
          dom_node.removeAttribute(this.psaNotProcessed_);
        }
      }
    }
  };
  deferJsNs.DeferJs.prototype.setNotProcessedAttributeForNodes = function() {
    var nodes = this.origGetElementsByTagName_.call(document, "*");
    for (var i = 0; i < nodes.length; i++) {
      var dom_node = nodes.item(i);
      dom_node.setAttribute(this.psaNotProcessed_, "");
    }
  };
  deferJsNs.DeferJs.prototype.nextPsaJsNode = function() {
    var current_node = null;
    if (document.querySelector) {
      current_node = document.querySelector(
        '[type="' + this.psaScriptType_ + '"]'
      );
    }
    return current_node;
  };
  deferJsNs.DeferJs.prototype.getCurrentDomLocation = function() {
    var current_node;
    if (document.querySelector) {
      current_node = document.querySelector(
        "[" + deferJsNs.DeferJs.PSA_CURRENT_NODE + "]"
      );
    }
    return current_node || this.origGetElementsByTagName_.call(document, "psanode")[0];
  };
  deferJsNs.DeferJs.prototype.removeCurrentDomLocation = function() {
    var oldNode = this.getCurrentDomLocation();
    if (oldNode.nodeName == "SCRIPT") {
      oldNode.parentNode.removeChild(oldNode);
    }
  };
  deferJsNs.DeferJs.prototype.onComplete = function() {
    if (this.state_ >= deferJsNs.DeferJs.STATES.WAITING_FOR_ONLOAD) {
      return;
    }
    if (this.lastIncrementalRun_ && this.isLowPriorityDeferJs()) {
      if (this.getIEVersion() && document.documentElement["originalDoScroll"]) {
        document.documentElement.doScroll = document.documentElement["originalDoScroll"];
      }
      if (Object.defineProperty) {
        delete document["readyState"];
      }
      if (this.getIEVersion()) {
        if (Object.defineProperty) {
          delete document["all"];
        }
      }
    }
    this.overrideDefaultImplementation_ = false;
    if (this.lastIncrementalRun_) {
      this.state_ = deferJsNs.DeferJs.STATES.WAITING_FOR_ONLOAD;
      if (this.isLowPriorityDeferJs()) {
        var me = this;
        if (document.readyState != "complete") {
          deferJsNs.addOnload(window, function() {
            me.fireOnload();
          });
        } else {
          if (document.onreadystatechange) {
            this.exec(document.onreadystatechange, document);
          }
          if (window.onload) {
            psaAddEventListener(window, "onload", window.onload);
            window.onload = null;
          }
          this.fireOnload();
        }
      } else {
        this.highPriorityFinalize();
      }
    } else {
      this.state_ = deferJsNs.DeferJs.STATES.WAITING_FOR_NEXT_RUN;
      this.firstIncrementalRun_ = false;
      if (this.incrementalScriptsDoneCallback_ && this.isLowPriorityDeferJs()) {
        pagespeed.deferJs = pagespeed.highPriorityDeferJs;
        pagespeed["deferJs"] = pagespeed.highPriorityDeferJs;
        this.exec(this.incrementalScriptsDoneCallback_);
        this.incrementalScriptsDoneCallback_ = null;
      } else {
        this.highPriorityFinalize();
      }
    }
  };
  deferJsNs.DeferJs.prototype.fireOnload = function() {
    if (this.isLowPriorityDeferJs()) {
      this.addDeferredOnloadListeners();
      pagespeed.highPriorityDeferJs.fireOnload();
    }
    this.fireEvent(deferJsNs.DeferJs.EVENT.LOAD);
    if (this.isLowPriorityDeferJs()) {
      var psanodes = document.body.getElementsByTagName("psanode");
      for (var i = psanodes.length - 1; i >= 0; i--) {
        document.body.removeChild(psanodes[i]);
      }
      var prefetchContainers = document.body.getElementsByClassName("psa_prefetch_container");
      for (var i = prefetchContainers.length - 1; i >= 0; i--) {
        prefetchContainers[i].parentNode.removeChild(prefetchContainers[i]);
      }
    }
    this.state_ = deferJsNs.DeferJs.STATES.SCRIPTS_DONE;
    this.fireEvent(deferJsNs.DeferJs.EVENT.AFTER_SCRIPTS);
  };
  deferJsNs.DeferJs.prototype.highPriorityFinalize = function() {
    var me = this;
    window.setTimeout(function() {
      pagespeed.deferJs = pagespeed.lowPriorityDeferJs;
      pagespeed["deferJs"] = pagespeed.lowPriorityDeferJs;
      if (me.incrementalScriptsDoneCallback_) {
        pagespeed.deferJs.registerScriptTags(
          me.incrementalScriptsDoneCallback_,
          me.optLastIndex_
        );
        me.incrementalScriptsDoneCallback_ = null;
      } else {
        pagespeed.deferJs.registerScriptTags();
      }
      pagespeed.deferJs.execute();
    }, 0);
  };
  deferJsNs.DeferJs.prototype.checkNodeInDom = function(node) {
    while (node = node.parentNode) {
      if (node == document) {
        return true;
      }
    }
    return false;
  };
  deferJsNs.DeferJs.prototype.getNumScriptsWithNoOnload = function(dynamicInsertedScriptList) {
    var count = 0;
    var len = dynamicInsertedScriptList.length;
    for (var i = 0; i < len; ++i) {
      var node = dynamicInsertedScriptList[i];
      var parent = node.parentNode;
      var src = node.src;
      var text = node.textContent;
      if (this.getIEVersion() > 8 && (!parent || src == "" && text == "") || !this.getIEVersion() && (!this.checkNodeInDom(node) || src == "")) {
        count++;
      }
    }
    return count;
  };
  deferJsNs.DeferJs.prototype.canCallOnComplete = function() {
    if (this.state_ != deferJsNs.DeferJs.STATES.SYNC_SCRIPTS_DONE) {
      return false;
    }
    var count = 0;
    if (this.dynamicInsertedScriptCount_ != 0) {
      count = this.getNumScriptsWithNoOnload(this.dynamicInsertedScript_);
    }
    if (this.dynamicInsertedScriptCount_ == count) {
      return true;
    }
    return false;
  };
  deferJsNs.DeferJs.prototype.scriptsAreDone = function() {
    return this.state_ === deferJsNs.DeferJs.STATES.SCRIPTS_DONE;
  };
  deferJsNs.DeferJs.prototype["scriptsAreDone"] = deferJsNs.DeferJs.prototype.scriptsAreDone;
  deferJsNs.DeferJs.prototype.runNext = function() {
    this.handlePendingDocumentWrites();
    this.removeCurrentDomLocation();
    if (this.next_ < this.queue_.length) {
      this.next_++;
      this.queue_[this.next_ - 1].call(window);
    } else {
      if (this.lastIncrementalRun_) {
        this.state_ = deferJsNs.DeferJs.STATES.SYNC_SCRIPTS_DONE;
        this.removeNotProcessedAttributeTillNode();
        this.fireEvent(deferJsNs.DeferJs.EVENT.DOM_READY);
        if (this.canCallOnComplete()) {
          this.onComplete();
        }
      } else {
        this.onComplete();
      }
    }
  };
  deferJsNs.DeferJs.prototype.nodeListToArray = function(nodeList) {
    var arr = [];
    var len = nodeList.length;
    for (var i = 0; i < len; ++i) {
      arr.push(nodeList.item(i));
    }
    return arr;
  };
  deferJsNs.DeferJs.prototype.setUp = function() {
    var me = this;
    if (this.firstIncrementalRun_ && !this.isLowPriorityDeferJs()) {
      var initialContextNode = document.createElement("psanode");
      initialContextNode.setAttribute("psa_dw_target", "true");
      document.body.appendChild(initialContextNode);
      if (this.getIEVersion()) {
        this.createIdVars();
      }
      if (Object.defineProperty) {
        try {
          var propertyDescriptor = { configurable: true };
          propertyDescriptor.get = function() {
            return me.state_ >= deferJsNs.DeferJs.STATES.SYNC_SCRIPTS_DONE ? "interactive" : "loading";
          };
          Object.defineProperty(document, "readyState", propertyDescriptor);
        } catch (err) {
          this.log("Exception while overriding document.readyState.", err);
        }
      }
      if (this.getIEVersion()) {
        document.documentElement["originalDoScroll"] = document.documentElement.doScroll;
        document.documentElement.doScroll = function() {
          throw "psa exception";
        };
        if (Object.defineProperty) {
          try {
            var propertyDescriptor = { configurable: true };
            propertyDescriptor.get = function() {
              return void 0;
            };
            Object.defineProperty(document, "all", propertyDescriptor);
          } catch (err) {
            this.log("Exception while overriding document.all.", err);
          }
        }
      }
    }
    this.overrideAddEventListeners();
    document.writeln = function(x) {
      me.writeHtml(x + "\n");
    };
    document.write = function(x) {
      me.writeHtml(x);
    };
    document.open = function() {
      if (!me.overrideDefaultImplementation_) {
        me.origDocOpen_.call(document);
      }
    };
    document.close = function() {
      if (!me.overrideDefaultImplementation_) {
        me.origDocClose_.call(document);
      }
    };
    document.getElementById = function(str) {
      me.handlePendingDocumentWrites();
      var node = me.origGetElementById_.call(document, str);
      return node == null || node.hasAttribute(me.psaNotProcessed_) ? null : node;
    };
    if (document.querySelectorAll && !(me.getIEVersion() <= 8)) {
      document.getElementsByTagName = function(tagName) {
        if (me.overrideDefaultImplementation_) {
          try {
            return document.querySelectorAll(
              tagName + ":not([" + me.psaNotProcessed_ + "])"
            );
          } catch (err) {
          }
        }
        return me.origGetElementsByTagName_.call(document, tagName);
      };
    }
    document.createElement = function(str) {
      var elem = me.origCreateElement_.call(document, str);
      if (me.overrideDefaultImplementation_ && str.toLowerCase() == "script") {
        me.dynamicInsertedScript_.push(elem);
        me.dynamicInsertedScriptCount_++;
        var onload = function() {
          me.dynamicInsertedScriptCount_--;
          var index = me.dynamicInsertedScript_.indexOf(this);
          if (index != -1) {
            me.dynamicInsertedScript_.splice(index, 1);
            if (me.canCallOnComplete()) {
              me.onComplete();
            }
          }
        };
        deferJsNs.addOnload(elem, onload);
        addHandler(elem, "error", onload);
      }
      return elem;
    };
  };
  deferJsNs.DeferJs.prototype.execute = function() {
    if (this.state_ != deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED) {
      return;
    }
    var count = 0;
    if (this.noDeferAsyncScriptsCount_ != 0) {
      count = this.getNumScriptsWithNoOnload(this.noDeferAsyncScripts_);
    }
    if (this.noDeferAsyncScriptsCount_ == count) {
      this.run();
    }
  };
  deferJsNs.DeferJs.prototype["execute"] = deferJsNs.DeferJs.prototype.execute;
  deferJsNs.DeferJs.prototype.run = function() {
    if (this.state_ != deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED) {
      return;
    }
    if (this.firstIncrementalRun_) {
      this.fireEvent(deferJsNs.DeferJs.EVENT.BEFORE_SCRIPTS);
    }
    this.state_ = deferJsNs.DeferJs.STATES.SCRIPTS_EXECUTING;
    this.setUp();
    this.runNext();
  };
  deferJsNs.DeferJs.prototype["run"] = deferJsNs.DeferJs.prototype.run;
  deferJsNs.DeferJs.prototype.parseHtml = function(html) {
    var div = this.origCreateElement_.call(document, "div");
    div.innerHTML = "<div>_</div>" + html;
    div.removeChild(div.firstChild);
    return div;
  };
  deferJsNs.DeferJs.prototype.disown = function(node) {
    var parentNode = node.parentNode;
    if (parentNode) {
      parentNode.removeChild(node);
    }
  };
  deferJsNs.DeferJs.prototype.insertNodesBeforeElem = function(nodes, elem) {
    var nodeArray = this.nodeListToArray(nodes);
    var len = nodeArray.length;
    var parentNode = elem.parentNode;
    for (var i = 0; i < len; ++i) {
      var node = nodeArray[i];
      this.disown(node);
      parentNode.insertBefore(node, elem);
    }
  };
  deferJsNs.DeferJs.prototype.isJSNode = function(node) {
    if (node.nodeName != "SCRIPT") {
      return false;
    }
    if (node.hasAttribute("type")) {
      var type = node.getAttribute("type");
      return !type || this.jsMimeTypes.indexOf(type) != -1;
    } else if (node.hasAttribute("language")) {
      var lang = node.getAttribute("language");
      return !lang || this.jsMimeTypes.indexOf("text/" + lang.toLowerCase()) != -1;
    }
    return true;
  };
  deferJsNs.DeferJs.prototype.markNodesAndExtractScriptNodes = function(node, scriptNodes) {
    if (!node.childNodes) {
      return;
    }
    var nodeArray = this.nodeListToArray(node.childNodes);
    var len = nodeArray.length;
    for (var i = 0; i < len; ++i) {
      var child = nodeArray[i];
      if (child.nodeName == "SCRIPT") {
        if (this.isJSNode(child)) {
          scriptNodes.push(child);
          child.setAttribute(deferJsNs.DeferJs.PSA_ORIG_TYPE, child.type);
          child.setAttribute("type", this.psaScriptType_);
          child.setAttribute(deferJsNs.DeferJs.PSA_ORIG_SRC, child.src);
          child.setAttribute("src", "");
          child.setAttribute(this.psaNotProcessed_, "");
        }
      } else {
        this.markNodesAndExtractScriptNodes(child, scriptNodes);
      }
    }
  };
  deferJsNs.DeferJs.prototype.deferScripts = function(scripts, pos) {
    var len = scripts.length;
    for (var i = 0; i < len; ++i) {
      this.addNode(scripts[i], pos + i, !!i);
    }
  };
  deferJsNs.DeferJs.prototype.insertHtml = function(html, pos, opt_elem) {
    var node = this.parseHtml(html);
    var scriptNodes = [];
    this.markNodesAndExtractScriptNodes(node, scriptNodes);
    if (opt_elem) {
      this.insertNodesBeforeElem(node.childNodes, opt_elem);
    } else {
      this.log("Unable to insert nodes, no context element found");
    }
    this.deferScripts(scriptNodes, pos);
  };
  deferJsNs.DeferJs.prototype.handlePendingDocumentWrites = function() {
    if (this.documentWriteHtml_ == "") {
      return;
    }
    this.log("handle_dw: " + this.documentWriteHtml_);
    var html = this.documentWriteHtml_;
    this.documentWriteHtml_ = "";
    var currentElem = this.getCurrentDomLocation();
    this.insertHtml(html, this.next_, currentElem);
  };
  deferJsNs.DeferJs.prototype.writeHtml = function(html) {
    if (this.overrideDefaultImplementation_) {
      this.log("dw: " + html);
      this.documentWriteHtml_ += html;
    } else {
      this.origDocWrite_.call(document, html);
    }
  };
  deferJsNs.DeferJs.prototype.addDeferredOnloadListeners = function() {
    var onloadDeferredElements;
    if (document.querySelectorAll) {
      onloadDeferredElements = document.querySelectorAll(
        "[" + deferJsNs.DeferJs.PAGESPEED_ONLOAD + "][data-pagespeed-loaded]"
      );
    }
    for (var i = 0; i < onloadDeferredElements.length; i++) {
      var elem = onloadDeferredElements.item(i);
      var handlerStr = elem.getAttribute(deferJsNs.DeferJs.PAGESPEED_ONLOAD);
      var functionStr = "var psaFunc=function() {" + handlerStr + "};";
      window["eval"].call(window, functionStr);
      if (typeof window["psaFunc"] != "function") {
        this.log("Function is not defined", new Error(""));
        continue;
      }
      psaAddEventListener(elem, "onload", window["psaFunc"]);
    }
  };
  deferJsNs.DeferJs.prototype.addBeforeDeferRunFunctions = function(func) {
    psaAddEventListener(window, "onbeforescripts", func);
  };
  deferJsNs.DeferJs.prototype["addBeforeDeferRunFunctions"] = deferJsNs.DeferJs.prototype.addBeforeDeferRunFunctions;
  deferJsNs.DeferJs.prototype.addAfterDeferRunFunctions = function(func) {
    psaAddEventListener(window, "onafterscripts", func);
  };
  deferJsNs.DeferJs.prototype["addAfterDeferRunFunctions"] = deferJsNs.DeferJs.prototype.addAfterDeferRunFunctions;
  deferJsNs.DeferJs.prototype.fireEvent = function(evt) {
    this.eventState_ = evt;
    this.log("Firing Event: " + evt);
    var eventListeners = this.eventListenersMap_[evt] || [];
    for (var i = 0; i < eventListeners.length; ++i) {
      this.exec(eventListeners[i]);
    }
    eventListeners.length = 0;
  };
  deferJsNs.DeferJs.prototype.exec = function(func, opt_scopeObject) {
    try {
      func.call(opt_scopeObject || window);
    } catch (err) {
      this.log("Exception while evaluating.", err);
    }
  };
  deferJsNs.DeferJs.prototype.overrideAddEventListeners = function() {
    var me = this;
    if (window.addEventListener) {
      document.addEventListener = function(eventName, func, capture) {
        psaAddEventListener(
          document,
          eventName,
          func,
          me.origDocAddEventListener_,
          capture
        );
      };
      window.addEventListener = function(eventName, func, capture) {
        psaAddEventListener(
          window,
          eventName,
          func,
          me.origWindowAddEventListener_,
          capture
        );
      };
    } else if (window.attachEvent) {
      document.attachEvent = function(eventName, func) {
        psaAddEventListener(
          document,
          eventName,
          func,
          me.origDocAttachEvent_
        );
      };
      window.attachEvent = function(eventName, func) {
        psaAddEventListener(
          window,
          eventName,
          func,
          me.origWindowAttachEvent_
        );
      };
    }
  };
  var psaAddEventListener = function(elem, eventName, func, opt_originalAddEventListener, opt_capture) {
    var deferJs = pagespeed["deferJs"];
    if (deferJs.state_ >= deferJsNs.DeferJs.STATES.WAITING_FOR_ONLOAD) {
      if (opt_originalAddEventListener) {
        opt_originalAddEventListener.call(elem, eventName, func, opt_capture);
        return;
      }
      if (deferJs.state_ >= deferJsNs.DeferJs.STATES.SCRIPTS_DONE) {
        return;
      }
    }
    var deferJsEvent;
    var deferJsEventName;
    if (deferJs.eventState_ < deferJsNs.DeferJs.EVENT.DOM_READY && (eventName == "DOMContentLoaded" || eventName == "readystatechange" || eventName == "onDOMContentLoaded" || eventName == "onreadystatechange")) {
      deferJsEvent = deferJsNs.DeferJs.EVENT.DOM_READY;
      deferJsEventName = "DOMContentLoaded";
    } else if (deferJs.eventState_ < deferJsNs.DeferJs.EVENT.LOAD && (eventName == "load" || eventName == "onload")) {
      deferJsEvent = deferJsNs.DeferJs.EVENT.LOAD;
      deferJsEventName = "load";
    } else if (eventName == "onbeforescripts") {
      deferJsEvent = deferJsNs.DeferJs.EVENT.BEFORE_SCRIPTS;
    } else if (eventName == "onafterscripts") {
      deferJsEvent = deferJsNs.DeferJs.EVENT.AFTER_SCRIPTS;
    } else {
      if (opt_originalAddEventListener) {
        opt_originalAddEventListener.call(elem, eventName, func, opt_capture);
      }
      return;
    }
    var eventListenerClosure = function() {
      var customEvent = {};
      customEvent["bubbles"] = false;
      customEvent["cancelable"] = false;
      customEvent["eventPhase"] = 2;
      customEvent["timeStamp"] = (new Date()).getTime();
      customEvent["type"] = deferJsEventName;
      customEvent["target"] = elem != window ? elem : document;
      customEvent["currentTarget"] = elem;
      func.call(elem, customEvent);
    };
    if (!deferJs.eventListenersMap_[deferJsEvent]) {
      deferJs.eventListenersMap_[deferJsEvent] = [];
    }
    deferJs.eventListenersMap_[deferJsEvent].push(
      eventListenerClosure
    );
  };
  deferJsNs.DeferJs.prototype.registerScriptTags = function(opt_callback, opt_last_index) {
    if (this.state_ >= deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED) {
      return;
    }
    if (opt_callback) {
      if (!deferJsNs.DeferJs.isExperimentalMode) {
        opt_callback();
        return;
      }
      this.lastIncrementalRun_ = false;
      this.incrementalScriptsDoneCallback_ = opt_callback;
      if (opt_last_index) {
        this.optLastIndex_ = opt_last_index;
      }
    } else {
      this.lastIncrementalRun_ = true;
    }
    this.state_ = deferJsNs.DeferJs.STATES.SCRIPTS_REGISTERED;
    var scripts = document.getElementsByTagName("script");
    var len = scripts.length;
    for (var i = 0; i < len; ++i) {
      var isFirstScript = this.queue_.length == this.next_;
      var script = scripts[i];
      if (script.getAttribute("type") == this.psaScriptType_) {
        if (opt_callback) {
          var scriptIndex = script.getAttribute(deferJsNs.DeferJs.PSA_ORIG_INDEX);
          if (scriptIndex <= this.optLastIndex_) {
            this.addNode(script, void 0, !isFirstScript);
          }
        } else {
          if (script.getAttribute(deferJsNs.DeferJs.PSA_ORIG_INDEX) < this.optLastIndex_) {
            this.log(
              "Executing a script twice. Orig_Index: " + script.getAttribute(deferJsNs.DeferJs.PSA_ORIG_INDEX),
              new Error("")
            );
          }
          this.addNode(script, void 0, !isFirstScript);
        }
      }
    }
  };
  deferJsNs.DeferJs.prototype["registerScriptTags"] = deferJsNs.DeferJs.prototype.registerScriptTags;
  deferJsNs.addOnload = function(elem, func) {
    addHandler(elem, "load", func);
  };
  pagespeed["addOnload"] = deferJsNs.addOnload;
  deferJsNs.DeferJs.prototype.isWebKit = function() {
    return navigator.userAgent.indexOf("AppleWebKit") != -1;
  };
  deferJsNs.DeferJs.prototype.getIEVersion = function() {
    var version = /(?:MSIE.(\d+\.\d+))/.exec(navigator.userAgent);
    return version && version[1] ? document.documentMode || parseFloat(version[1]) : NaN;
  };
  deferJsNs.DeferJs.prototype.setType = function(type) {
    this.psaScriptType_ = type;
  };
  deferJsNs.DeferJs.prototype.setPsaNotProcessed = function(psaNotProcessed) {
    this.psaNotProcessed_ = psaNotProcessed;
  };
  deferJsNs.DeferJs.prototype.isLowPriorityDeferJs = function() {
    if (this.psaScriptType_ == deferJsNs.DeferJs.PSA_SCRIPT_TYPE) {
      return true;
    }
    return false;
  };
  deferJsNs.DeferJs.prototype.noDeferCreateElementOverride = function() {
    var me = this;
    document.createElement = function(str) {
      var elem = me.origCreateElement_.call(document, str);
      if (me.overrideDefaultImplementation_ && str.toLowerCase() == "script") {
        me.noDeferAsyncScripts_.push(elem);
        me.noDeferAsyncScriptsCount_++;
        var onload = function() {
          var index = me.noDeferAsyncScripts_.indexOf(this);
          if (index != -1) {
            me.noDeferAsyncScripts_.splice(index, 1);
            me.noDeferAsyncScriptsCount_--;
            me.execute();
          }
        };
        deferJsNs.addOnload(elem, onload);
        addHandler(elem, "error", onload);
      }
      return elem;
    };
  };
  deferJsNs.DeferJs.prototype.isExperimentalMode = function() {
    return deferJsNs.DeferJs.isExperimentalMode;
  };
  deferJsNs.DeferJs.prototype["isExperimentalMode"] = deferJsNs.DeferJs.prototype.isExperimentalMode;
  deferJsNs.deferInit = function() {
    if (pagespeed["deferJs"]) {
      return;
    }
    deferJsNs.DeferJs.isExperimentalMode = pagespeed["defer_js_experimental"];
    pagespeed.highPriorityDeferJs = new deferJsNs.DeferJs();
    pagespeed.highPriorityDeferJs.setType(
      deferJsNs.DeferJs.PRIORITY_PSA_SCRIPT_TYPE
    );
    pagespeed.highPriorityDeferJs.setPsaNotProcessed(
      deferJsNs.DeferJs.PRIORITY_PSA_NOT_PROCESSED
    );
    pagespeed.highPriorityDeferJs.setNotProcessedAttributeForNodes();
    pagespeed.lowPriorityDeferJs = new deferJsNs.DeferJs();
    pagespeed.lowPriorityDeferJs.setType(deferJsNs.DeferJs.PSA_SCRIPT_TYPE);
    pagespeed.lowPriorityDeferJs.setPsaNotProcessed(
      deferJsNs.DeferJs.PSA_NOT_PROCESSED
    );
    pagespeed.lowPriorityDeferJs.setNotProcessedAttributeForNodes();
    pagespeed.deferJs = pagespeed.highPriorityDeferJs;
    pagespeed.deferJs.noDeferCreateElementOverride();
    pagespeed["deferJs"] = pagespeed.deferJs;
  };
  deferJsNs.deferInit();
  pagespeed.deferJsStarted = false;
  deferJsNs.startDeferJs = function() {
    if (pagespeed.deferJsStarted) {
      return;
    }
    pagespeed.deferJsStarted = true;
    pagespeed.deferJs.registerScriptTags();
    pagespeed.deferJs.execute();
  };
  deferJsNs["startDeferJs"] = deferJsNs.startDeferJs;
  addHandler(document, "DOMContentLoaded", deferJsNs.startDeferJs);
  deferJsNs.addOnload(window, deferJsNs.startDeferJs);
})();
