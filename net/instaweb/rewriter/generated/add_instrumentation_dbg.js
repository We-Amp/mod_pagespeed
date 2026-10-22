(() => {
  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.AddInstrumentation = function(beaconUrlPrefix, extraParams, htmlUrl) {
    this.beaconUrlPrefix_ = beaconUrlPrefix;
    this.extraParams_ = extraParams;
    this.htmlUrl_ = htmlUrl;
    this.sent_ = false;
    this.lcpMs_ = -1;
    this.clsSession_ = { sum: 0, firstTime: 0, lastTime: 0 };
    this.cls_ = 0;
    this.clsObserved_ = false;
    this.inpMs_ = -1;
    this.observers_ = [];
    this.initObservers_();
  };
  pagespeed.AddInstrumentation.prototype.observe_ = function(type, handler, opt_extra) {
    var PO = window["PerformanceObserver"];
    if (!PO) {
      return false;
    }
    if (!PO["supportedEntryTypes"] || PO["supportedEntryTypes"].indexOf(type) == -1) {
      return false;
    }
    try {
      var observer = new PO(function(list) {
        handler(list["getEntries"]());
      });
      var opts = opt_extra || {};
      opts["type"] = type;
      opts["buffered"] = true;
      observer["observe"](opts);
      this.observers_.push({ observer, handler });
      return true;
    } catch (e) {
      return false;
    }
  };
  pagespeed.AddInstrumentation.prototype.initObservers_ = function() {
    var that = this;
    this.observe_("largest-contentful-paint", function(entries) {
      for (var i = 0; i < entries.length; i++) {
        var e = entries[i];
        that.lcpMs_ = Math.round(e["renderTime"] || e["loadTime"] || e["startTime"]);
      }
    });
    this.clsObserved_ = this.observe_("layout-shift", function(entries) {
      for (var i = 0; i < entries.length; i++) {
        var e = entries[i];
        if (e["hadRecentInput"]) {
          continue;
        }
        var s = that.clsSession_;
        if (s.sum > 0 && e["startTime"] - s.lastTime < 1e3 && e["startTime"] - s.firstTime < 5e3) {
          s.sum += e["value"];
          s.lastTime = e["startTime"];
        } else {
          s.sum = e["value"];
          s.firstTime = s.lastTime = e["startTime"];
        }
        if (s.sum > that.cls_) {
          that.cls_ = s.sum;
        }
      }
    });
    var inpHandler = function(entries) {
      for (var i = 0; i < entries.length; i++) {
        var e = entries[i];
        if (e["entryType"] == "event" && !e["interactionId"]) {
          continue;
        }
        var duration = Math.round(e["duration"]);
        if (duration > that.inpMs_) {
          that.inpMs_ = duration;
        }
      }
    };
    this.observe_("event", inpHandler, { "durationThreshold": 40 });
    this.observe_("first-input", inpHandler);
  };
  pagespeed.AddInstrumentation.prototype.rearm_ = function() {
    for (var i = 0; i < this.observers_.length; i++) {
      try {
        this.observers_[i].observer["disconnect"]();
      } catch (e) {
      }
    }
    this.observers_ = [];
    this.sent_ = false;
    this.lcpMs_ = -1;
    this.clsSession_ = { sum: 0, firstTime: 0, lastTime: 0 };
    this.cls_ = 0;
    this.inpMs_ = -1;
    this.initObservers_();
  };
  pagespeed.AddInstrumentation.prototype.buildBody_ = function() {
    var parts = [];
    var perf = window["performance"];
    var navEntry = null;
    if (perf && perf["getEntriesByType"]) {
      var navEntries = perf["getEntriesByType"]("navigation");
      if (navEntries && navEntries.length > 0) {
        navEntry = navEntries[0];
      }
    }
    if (navEntry) {
      var loadMs = Math.round(navEntry["loadEventStart"]);
      if (loadMs > 0) {
        parts.push("ets=load:" + loadMs);
        parts.push("rload=" + loadMs);
      }
      parts.push("nav=" + Math.round(navEntry["fetchStart"]));
      parts.push("dns=" + Math.round(navEntry["domainLookupEnd"] - navEntry["domainLookupStart"]));
      parts.push("connect=" + Math.round(navEntry["connectEnd"] - navEntry["connectStart"]));
      parts.push("req_start=" + Math.round(navEntry["requestStart"]));
      parts.push("c_ttfb=" + Math.round(navEntry["responseStart"]));
      parts.push("dwld=" + Math.round(navEntry["responseEnd"] - navEntry["responseStart"]));
      parts.push("dom_c=" + Math.round(navEntry["domContentLoadedEventStart"]));
      if (navEntry["type"]) {
        parts.push("nt=" + navEntry["type"]);
      }
    } else if (window["mod_pagespeed_start"]) {
      parts.push("ets=load:" + (Number(new Date()) - window["mod_pagespeed_start"]));
    }
    if (this.lcpMs_ >= 0) {
      parts.push("lcp=" + this.lcpMs_);
    }
    if (this.clsObserved_) {
      parts.push("cls=" + Math.round(this.cls_ * 1e3));
    }
    if (this.inpMs_ >= 0) {
      parts.push("inp=" + this.inpMs_);
    }
    parts.push("dpr=" + window.devicePixelRatio);
    var body = parts.join("&");
    if (this.extraParams_ != "") {
      body += this.extraParams_;
    }
    if (pagespeed.getResourceTimingData) {
      body += pagespeed.getResourceTimingData();
    }
    if (document.referrer) {
      body += "&ref=" + encodeURIComponent(document.referrer);
    }
    return body;
  };
  pagespeed.AddInstrumentation.prototype.send_ = function() {
    if (this.sent_) {
      return;
    }
    this.sent_ = true;
    for (var i = 0; i < this.observers_.length; i++) {
      var o = this.observers_[i];
      try {
        o.handler(o.observer["takeRecords"]());
        o.observer["disconnect"]();
      } catch (e) {
      }
    }
    var body = this.buildBody_();
    var url = this.beaconUrlPrefix_;
    url += url.indexOf("?") == -1 ? "?" : "&";
    url += "url=" + encodeURIComponent(this.htmlUrl_);
    var nav = window.navigator;
    if (nav && nav["sendBeacon"]) {
      try {
        if (nav["sendBeacon"](url, body)) {
          return;
        }
      } catch (e) {
      }
    }
    new Image().src = url + "&" + body;
  };
  pagespeed.addInstrumentationInit = function(beaconUrl, extraParams, htmlUrl) {
    if (window.parent != window) {
      return;
    }
    var collector = new pagespeed.AddInstrumentation(
      beaconUrl,
      extraParams,
      htmlUrl
    );
    document.addEventListener("visibilitychange", function() {
      if (document["visibilityState"] == "hidden") {
        collector.send_();
      }
    });
    window.addEventListener("pagehide", function() {
      collector.send_();
    });
    window.addEventListener("pageshow", function(event) {
      if (event["persisted"]) {
        collector.rearm_();
      }
    });
  };
})();
