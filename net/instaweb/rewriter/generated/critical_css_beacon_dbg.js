(() => {
  var MAX_POST_SIZE = 131072;
  var sendBeacon = function(beaconUrl, htmlUrl, data) {
    var httpRequest;
    if (window.XMLHttpRequest) {
      httpRequest = new XMLHttpRequest();
    } else if (window.ActiveXObject) {
      try {
        httpRequest = new ActiveXObject("Msxml2.XMLHTTP");
      } catch (e) {
        try {
          httpRequest = new ActiveXObject("Microsoft.XMLHTTP");
        } catch (e2) {
        }
      }
    }
    if (!httpRequest) {
      return false;
    }
    var query_param_char = beaconUrl.indexOf("?") == -1 ? "?" : "&";
    var url = beaconUrl + query_param_char + "url=" + encodeURIComponent(htmlUrl);
    httpRequest.open("POST", url);
    httpRequest.setRequestHeader(
      "Content-Type",
      "application/x-www-form-urlencoded"
    );
    httpRequest.send(data);
    return true;
  };
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

  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.CriticalCssBeacon = function(beaconUrl, htmlUrl, optionsHash, nonce, selectors, opt_aboveTheFoldOnly) {
    this.MAXITERS_ = 250;
    this.MAXMEASURES_ = 10;
    this.beaconUrl_ = beaconUrl;
    this.htmlUrl_ = htmlUrl;
    this.optionsHash_ = optionsHash;
    this.nonce_ = nonce;
    this.selectors_ = selectors;
    this.aboveTheFoldOnly_ = !!opt_aboveTheFoldOnly;
    this.criticalSelectors_ = [];
    this.idx_ = 0;
  };
  pagespeed.CriticalCssBeacon.prototype.sendBeacon_ = function() {
    var data = "oh=" + this.optionsHash_ + "&n=" + this.nonce_;
    data += "&cs=";
    var numSelectors = this.criticalSelectors_.length;
    var start = Math.floor(Math.random() * numSelectors);
    var maxDataLen = MAX_POST_SIZE - "&of=1".length;
    var truncated = false;
    for (var i = 0; i < numSelectors; ++i) {
      var tmp = i > 0 ? "," : "";
      tmp += encodeURIComponent(
        this.criticalSelectors_[(start + i) % numSelectors]
      );
      if (data.length + tmp.length > maxDataLen) {
        truncated = true;
        break;
      }
      data += tmp;
    }
    if (truncated) {
      data += "&of=1";
    }
    pagespeed.criticalCssBeaconData = data;
    sendBeacon(this.beaconUrl_, this.htmlUrl_, data);
  };
  pagespeed.CriticalCssBeacon.prototype.isSelectorCritical_ = function(selector) {
    var elements = document.querySelectorAll(selector);
    if (elements.length == 0) {
      return false;
    }
    if (!this.aboveTheFoldOnly_) {
      return true;
    }
    var viewportHeight = window.innerHeight || document.documentElement.clientHeight;
    if (!viewportHeight || !elements[0].getBoundingClientRect) {
      return true;
    }
    var scrollY = window.pageYOffset || 0;
    var numToMeasure = Math.min(elements.length, this.MAXMEASURES_);
    for (var i = 0; i < numToMeasure; ++i) {
      var rect = elements[i].getBoundingClientRect();
      if (rect.width == 0 && rect.height == 0) {
        return true;
      }
      if (rect.top + scrollY < viewportHeight) {
        return true;
      }
    }
    return elements.length > numToMeasure;
  };
  pagespeed.CriticalCssBeacon.prototype.checkCssSelectors_ = function(callback) {
    for (var i = 0; i < this.MAXITERS_ && this.idx_ < this.selectors_.length; ++i, ++this.idx_) {
      try {
        if (this.isSelectorCritical_(this.selectors_[this.idx_])) {
          this.criticalSelectors_.push(this.selectors_[this.idx_]);
        }
      } catch (e) {
        if (!this.aboveTheFoldOnly_) {
          this.criticalSelectors_.push(this.selectors_[this.idx_]);
        }
        continue;
      }
    }
    if (this.idx_ < this.selectors_.length) {
      window.setTimeout(this.checkCssSelectors_.bind(this), 0, callback);
    } else {
      callback();
    }
  };
  pagespeed.criticalCssBeaconInit = function(beaconUrl, htmlUrl, optionsHash, nonce, selectors, opt_aboveTheFoldOnly) {
    if (!document.querySelector || !document.querySelectorAll || !Function.prototype.bind) {
      return;
    }
    var temp = new pagespeed.CriticalCssBeacon(
      beaconUrl,
      htmlUrl,
      optionsHash,
      nonce,
      selectors,
      opt_aboveTheFoldOnly
    );
    var beacon_onload = function() {
      window.setTimeout(function() {
        temp.checkCssSelectors_(function() {
          temp.sendBeacon_();
        });
      }, 0);
    };
    addHandler(window, "load", beacon_onload);
  };
})();
