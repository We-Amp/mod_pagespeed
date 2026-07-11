(function(){var pagespeedutils = {MAX_POST_SIZE:131072, sendBeacon:function(a, b, d) {
  if (window.XMLHttpRequest) {
    var c = new XMLHttpRequest();
  } else if (window.ActiveXObject) {
    try {
      c = new ActiveXObject("Msxml2.XMLHTTP");
    } catch (e) {
      try {
        c = new ActiveXObject("Microsoft.XMLHTTP");
      } catch (g) {
      }
    }
  }
  if (!c) {
    return !1;
  }
  var f = a.indexOf("?") == -1 ? "?" : "&";
  a = a + f + "url=" + encodeURIComponent(b);
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
pagespeed.CriticalCssBeacon = function(a, b, d, c, f) {
  this.MAXITERS_ = 250;
  this.MAXMEASURES_ = 10;
  this.beaconUrl_ = a;
  this.htmlUrl_ = b;
  this.optionsHash_ = d;
  this.nonce_ = c;
  this.selectors_ = f;
  this.criticalSelectors_ = [];
  this.idx_ = 0;
};
pagespeed.CriticalCssBeacon.prototype.sendBeacon_ = function() {
  var a = "oh=" + this.optionsHash_ + "&n=" + this.nonce_;
  a += "&cs=";
  for (var b = this.criticalSelectors_.length, d = Math.floor(Math.random() * b), c = pagespeedutils.MAX_POST_SIZE - 5, f = !1, e = 0; e < b; ++e) {
    var g = e > 0 ? "," : "";
    g += encodeURIComponent(this.criticalSelectors_[(d + e) % b]);
    if (a.length + g.length > c) {
      f = !0;
      break;
    }
    a += g;
  }
  f && (a += "&of=1");
  pagespeed.criticalCssBeaconData = a;
  pagespeedutils.sendBeacon(this.beaconUrl_, this.htmlUrl_, a);
};
pagespeed.CriticalCssBeacon.prototype.isSelectorCritical_ = function(a) {
  a = document.querySelectorAll(a);
  if (a.length == 0) {
    return !1;
  }
  var b = window.innerHeight || document.documentElement.clientHeight;
  if (!b || !a[0].getBoundingClientRect) {
    return !0;
  }
  for (var d = window.pageYOffset || 0, c = Math.min(a.length, this.MAXMEASURES_), f = 0; f < c; ++f) {
    var e = a[f].getBoundingClientRect();
    if (e.width == 0 && e.height == 0 || e.top + d < b) {
      return !0;
    }
  }
  return a.length > c;
};
pagespeed.CriticalCssBeacon.prototype.checkCssSelectors_ = function(a) {
  for (var b = 0; b < this.MAXITERS_ && this.idx_ < this.selectors_.length; ++b, ++this.idx_) {
    try {
      this.isSelectorCritical_(this.selectors_[this.idx_]) && this.criticalSelectors_.push(this.selectors_[this.idx_]);
    } catch (d) {
    }
  }
  this.idx_ < this.selectors_.length ? window.setTimeout(this.checkCssSelectors_.bind(this), 0, a) : a();
};
pagespeed.criticalCssBeaconInit = function(a, b, d, c, f) {
  if (document.querySelector && document.querySelectorAll && Function.prototype.bind) {
    var e = new pagespeed.CriticalCssBeacon(a, b, d, c, f);
    pagespeedutils.addHandler(window, "load", function() {
      window.setTimeout(function() {
        e.checkCssSelectors_(function() {
          e.sendBeacon_();
        });
      }, 0);
    });
  }
};
pagespeed.criticalCssBeaconInit = pagespeed.criticalCssBeaconInit;
})();
