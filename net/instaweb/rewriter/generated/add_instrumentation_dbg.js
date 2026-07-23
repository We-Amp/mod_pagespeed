(function(){window.pagespeed = window.pagespeed || {};
var pagespeed = window.pagespeed;
pagespeed.AddInstrumentation = function(a, c, b) {
  this.beaconUrlPrefix_ = a;
  this.extraParams_ = c;
  this.htmlUrl_ = b;
  this.sent_ = !1;
  this.lcpMs_ = -1;
  this.clsSession_ = {sum:0, firstTime:0, lastTime:0};
  this.cls_ = 0;
  this.clsObserved_ = !1;
  this.inpMs_ = -1;
  this.observers_ = [];
  this.initObservers_();
};
pagespeed.AddInstrumentation.prototype.observe_ = function(a, c, b) {
  var e = window.PerformanceObserver;
  if (!e || !e.supportedEntryTypes || e.supportedEntryTypes.indexOf(a) == -1) {
    return !1;
  }
  try {
    var d = new e(function(f) {
      c(f.getEntries());
    });
    b = b || {};
    b.type = a;
    b.buffered = !0;
    d.observe(b);
    this.observers_.push({observer:d, handler:c});
    return !0;
  } catch (f) {
    return !1;
  }
};
pagespeed.AddInstrumentation.prototype.initObservers_ = function() {
  var a = this;
  this.observe_("largest-contentful-paint", function(b) {
    for (var e = 0; e < b.length; e++) {
      var d = b[e];
      a.lcpMs_ = Math.round(d.renderTime || d.loadTime || d.startTime);
    }
  });
  this.clsObserved_ = this.observe_("layout-shift", function(b) {
    for (var e = 0; e < b.length; e++) {
      var d = b[e];
      if (!d.hadRecentInput) {
        var f = a.clsSession_;
        f.sum > 0 && d.startTime - f.lastTime < 1000 && d.startTime - f.firstTime < 5000 ? (f.sum += d.value, f.lastTime = d.startTime) : (f.sum = d.value, f.firstTime = f.lastTime = d.startTime);
        f.sum > a.cls_ && (a.cls_ = f.sum);
      }
    }
  });
  var c = function(b) {
    for (var e = 0; e < b.length; e++) {
      var d = b[e];
      if (d.entryType != "event" || d.interactionId) {
        d = Math.round(d.duration), d > a.inpMs_ && (a.inpMs_ = d);
      }
    }
  };
  this.observe_("event", c, {durationThreshold:40});
  this.observe_("first-input", c);
};
pagespeed.AddInstrumentation.prototype.rearm_ = function() {
  for (var a = 0; a < this.observers_.length; a++) {
    try {
      this.observers_[a].observer.disconnect();
    } catch (c) {
    }
  }
  this.observers_ = [];
  this.sent_ = !1;
  this.lcpMs_ = -1;
  this.clsSession_ = {sum:0, firstTime:0, lastTime:0};
  this.cls_ = 0;
  this.inpMs_ = -1;
  this.initObservers_();
};
pagespeed.AddInstrumentation.prototype.buildBody_ = function() {
  var a = [], c = window.performance, b = null;
  c && c.getEntriesByType && (c = c.getEntriesByType("navigation")) && c.length > 0 && (b = c[0]);
  b ? (c = Math.round(b.loadEventStart), c > 0 && (a.push("ets=load:" + c), a.push("rload=" + c)), a.push("nav=" + Math.round(b.fetchStart)), a.push("dns=" + Math.round(b.domainLookupEnd - b.domainLookupStart)), a.push("connect=" + Math.round(b.connectEnd - b.connectStart)), a.push("req_start=" + Math.round(b.requestStart)), a.push("c_ttfb=" + Math.round(b.responseStart)), a.push("dwld=" + Math.round(b.responseEnd - b.responseStart)), a.push("dom_c=" + Math.round(b.domContentLoadedEventStart)), b.type && 
  a.push("nt=" + b.type)) : window.mod_pagespeed_start && a.push("ets=load:" + (Number(new Date()) - window.mod_pagespeed_start));
  this.lcpMs_ >= 0 && a.push("lcp=" + this.lcpMs_);
  this.clsObserved_ && a.push("cls=" + Math.round(this.cls_ * 1000));
  this.inpMs_ >= 0 && a.push("inp=" + this.inpMs_);
  a.push("dpr=" + window.devicePixelRatio);
  a = a.join("&");
  this.extraParams_ != "" && (a += this.extraParams_);
  pagespeed.getResourceTimingData && (a += pagespeed.getResourceTimingData());
  document.referrer && (a += "&ref=" + encodeURIComponent(document.referrer));
  return a;
};
pagespeed.AddInstrumentation.prototype.send_ = function() {
  if (!this.sent_) {
    this.sent_ = !0;
    for (var a = 0; a < this.observers_.length; a++) {
      var c = this.observers_[a];
      try {
        c.handler(c.observer.takeRecords()), c.observer.disconnect();
      } catch (e) {
      }
    }
    a = this.buildBody_();
    c = this.beaconUrlPrefix_;
    c += c.indexOf("?") == -1 ? "?" : "&";
    c += "url=" + encodeURIComponent(this.htmlUrl_);
    var b = window.navigator;
    if (b && b.sendBeacon) {
      try {
        if (b.sendBeacon(c, a)) {
          return;
        }
      } catch (e) {
      }
    }
    (new Image()).src = c + "&" + a;
  }
};
pagespeed.addInstrumentationInit = function(a, c, b) {
  if (window.parent == window) {
    var e = new pagespeed.AddInstrumentation(a, c, b);
    document.addEventListener("visibilitychange", function() {
      document.visibilityState == "hidden" && e.send_();
    });
    window.addEventListener("pagehide", function() {
      e.send_();
    });
    window.addEventListener("pageshow", function(d) {
      d.persisted && e.rearm_();
    });
  }
};
pagespeed.addInstrumentationInit = pagespeed.addInstrumentationInit;
})();
