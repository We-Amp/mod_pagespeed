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
  var getWindowSize = function() {
    var height = window.innerHeight || document.documentElement.clientHeight || document.body.clientHeight;
    var width = window.innerWidth || document.documentElement.clientWidth || document.body.clientWidth;
    return {
      height,
      width
    };
  };
  var now = Date.now || (function() {
    return +new Date();
  });

  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.CriticalImages = pagespeed.CriticalImages || {};
  pagespeed.CriticalImages.Beacon_ = function(beaconUrl, htmlUrl, optionsHash, checkRenderedImageSizes, nonce) {
    this.beaconUrl_ = beaconUrl;
    this.htmlUrl_ = htmlUrl;
    this.optionsHash_ = optionsHash;
    this.nonce_ = nonce;
    this.windowSize_ = getWindowSize();
    this.checkRenderedImageSizes_ = checkRenderedImageSizes;
    this.imgLocations_ = {};
    this.criticalImages_ = [];
    this.criticalImagesKeys_ = {};
  };
  pagespeed.CriticalImages.Beacon_.prototype.elLocation_ = function(element) {
    var rect = element.getBoundingClientRect();
    var body = document.body;
    var scrollX = "pageXOffset" in window ? window.pageXOffset : (document.documentElement || body.parentNode || body).scrollLeft;
    var scrollY = "pageYOffset" in window ? window.pageYOffset : (document.documentElement || body.parentNode || body).scrollTop;
    return {
      top: rect.top + scrollY,
      left: rect.left + scrollX
    };
  };
  pagespeed.CriticalImages.Beacon_.prototype.isCritical_ = function(element) {
    if (element.offsetWidth <= 0 && element.offsetHeight <= 0) {
      return false;
    }
    var elLocation = this.elLocation_(element);
    var elLocationStr = elLocation.top.toString() + "," + elLocation.left;
    if (this.imgLocations_.hasOwnProperty(elLocationStr)) {
      return false;
    } else {
      this.imgLocations_[elLocationStr] = true;
    }
    return elLocation.top <= this.windowSize_.height && elLocation.left <= this.windowSize_.width;
  };
  pagespeed.CriticalImages.isHtmlSpace_ = function(c) {
    return c == " " || c == "	" || c == "\n" || c == "\f" || c == "\r";
  };
  pagespeed.CriticalImages.parseSrcSetUrls_ = function(srcset) {
    var urls = [];
    var i = 0;
    var n = srcset.length;
    while (true) {
      while (i < n && (pagespeed.CriticalImages.isHtmlSpace_(srcset.charAt(i)) || srcset.charAt(i) == ",")) {
        ++i;
      }
      if (i >= n) {
        break;
      }
      var end = i;
      while (end < n && !pagespeed.CriticalImages.isHtmlSpace_(
        srcset.charAt(end)
      )) {
        ++end;
      }
      var url = srcset.substring(i, end);
      i = end;
      var expectDescriptor = true;
      while (url.charAt(url.length - 1) == ",") {
        url = url.substring(0, url.length - 1);
        expectDescriptor = false;
      }
      if (expectDescriptor) {
        var insideParen = false;
        while (i < n) {
          var c = srcset.charAt(i);
          if (c == "(") {
            insideParen = true;
          } else if (c == ")" && insideParen) {
            insideParen = false;
          } else if (c == "," && !insideParen) {
            break;
          }
          ++i;
        }
      }
      urls.push(url);
    }
    return urls;
  };
  pagespeed.CriticalImages.absolutifyUrl_ = function(url) {
    var a = document.createElement("a");
    a.href = url;
    return a.href;
  };
  pagespeed.CriticalImages.Beacon_.prototype.imageKey_ = function(element) {
    var key = element.getAttribute("data-pagespeed-url-hash");
    if (key) {
      return key;
    }
    var hashes = element.getAttribute("data-pagespeed-srcset-url-hashes");
    if (!hashes) {
      return null;
    }
    var currentSrc = element.currentSrc;
    if (!currentSrc) {
      return null;
    }
    var candidates = pagespeed.CriticalImages.parseSrcSetUrls_(
      element.getAttribute("srcset") || ""
    );
    var hashList = hashes.split(",");
    for (var i = 0; i < candidates.length && i < hashList.length; ++i) {
      if (hashList[i] && pagespeed.CriticalImages.absolutifyUrl_(candidates[i]) == currentSrc) {
        return hashList[i];
      }
    }
    return null;
  };
  pagespeed.CriticalImages.Beacon_.prototype.insertIfImageIsCritical_ = function(element) {
    var key = this.imageKey_(element);
    if (key && !(key in this.criticalImagesKeys_) && this.isCritical_(element)) {
      this.criticalImages_.push(key);
      this.criticalImagesKeys_[key] = true;
    }
  };
  pagespeed.CriticalImages.Beacon_.prototype.checkImageForCriticality = function(element) {
    if (element.getBoundingClientRect) {
      this.insertIfImageIsCritical_(element);
    }
  };
  pagespeed.CriticalImages.checkImageForCriticality = function(element) {
    pagespeed.CriticalImages.beaconObj_.checkImageForCriticality(element);
  };
  pagespeed.CriticalImages.checkCriticalImages = function() {
    pagespeed.CriticalImages.beaconObj_.checkCriticalImages_();
  };
  pagespeed.CriticalImages.Beacon_.prototype.checkCriticalImages_ = function() {
    this.imgLocations_ = {};
    var tags = ["IMG", "INPUT"];
    var elemsToCheck = [];
    for (var i = 0; i < tags.length; ++i) {
      elemsToCheck = elemsToCheck.concat(
        Array.from(document.getElementsByTagName(tags[i]))
      );
    }
    if (elemsToCheck.length == 0) {
      return;
    }
    if (!elemsToCheck[0].getBoundingClientRect) {
      return;
    }
    for (var i = 0, element; element = elemsToCheck[i]; ++i) {
      this.insertIfImageIsCritical_(element);
    }
    var data = "oh=" + this.optionsHash_;
    if (this.nonce_) {
      data += "&n=" + this.nonce_;
    }
    var isDataAvailable = this.criticalImages_.length != 0;
    if (isDataAvailable) {
      data += "&ci=" + encodeURIComponent(this.criticalImages_[0]);
      for (var i = 1; i < this.criticalImages_.length; ++i) {
        var tmp = "," + encodeURIComponent(this.criticalImages_[i]);
        if (data.length + tmp.length <= MAX_POST_SIZE) {
          data += tmp;
        }
      }
    }
    if (this.checkRenderedImageSizes_) {
      var tmp = "&rd=" + encodeURIComponent(JSON.stringify(this.getImageRenderedMap()));
      if (data.length + tmp.length <= MAX_POST_SIZE) {
        data += tmp;
      }
      isDataAvailable = true;
    }
    pagespeed.CriticalImages.beaconData_ = data;
    if (isDataAvailable) {
      sendBeacon(this.beaconUrl_, this.htmlUrl_, data);
    }
  };
  pagespeed.CriticalImages.Beacon_.prototype.getImageRenderedMap = function() {
    var renderedImageDimensions = {};
    var images = document.getElementsByTagName("IMG");
    if (images.length == 0) {
      return {};
    }
    var img = images[0];
    if (!("naturalWidth" in img) || !("naturalHeight" in img)) {
      return {};
    }
    for (var i = 0; img = images[i]; ++i) {
      var key = this.imageKey_(img);
      if (!key) {
        continue;
      }
      if (!(key in renderedImageDimensions) && img.width > 0 && img.height > 0 && img.naturalWidth > 0 && img.naturalHeight > 0 || key in renderedImageDimensions && img.width >= renderedImageDimensions[key].rw && img.height >= renderedImageDimensions[key].rh) {
        renderedImageDimensions[key] = {
          "rw": img.width,
          "rh": img.height,
          "ow": img.naturalWidth,
          "oh": img.naturalHeight
        };
      }
    }
    return renderedImageDimensions;
  };
  pagespeed.CriticalImages.beaconData_ = "";
  pagespeed.CriticalImages.getBeaconData = function() {
    return pagespeed.CriticalImages.beaconData_;
  };
  pagespeed.CriticalImages.Run = function(beaconUrl, htmlUrl, optionsHash, sendBeaconAtOnload, checkRenderedImageSizes, nonce) {
    var beacon = new pagespeed.CriticalImages.Beacon_(
      beaconUrl,
      htmlUrl,
      optionsHash,
      checkRenderedImageSizes,
      nonce
    );
    pagespeed.CriticalImages.beaconObj_ = beacon;
    if (sendBeaconAtOnload) {
      var beaconOnload = function() {
        window.setTimeout(function() {
          beacon.checkCriticalImages_();
        }, 0);
      };
      addHandler(window, "load", beaconOnload);
    }
  };
})();
