(() => {
  var toNumber = function(str) {
    var num = Number(str);
    if (num == 0 && /^[\s\xa0]*$/.test(str)) {
      return NaN;
    }
    return num;
  };
  var getPixelRatio = function() {
    return window.devicePixelRatio !== void 0 ? window.devicePixelRatio : 1;
  };

  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.ResponsiveImageCandidate = function(resolution, url) {
    this.resolution = resolution;
    this.url = url;
  };
  pagespeed.ResponsiveImage = function(img) {
    this.img = img;
    this.currentResolution = 0;
    this.availableResolutions = [];
  };
  pagespeed.Responsive = function() {
    this.allImages_ = [];
  };
  pagespeed.Responsive.updateImgSrc_ = function(img, url) {
    var tempImg = new Image();
    tempImg.onload = function() {
      img.src = url;
    };
    tempImg.src = url;
  };
  pagespeed.ResponsiveImage.prototype.responsiveResize = function(devicePixelRatio) {
    if (devicePixelRatio > this.currentResolution) {
      var numResolutions = this.availableResolutions.length;
      for (var i = 0; i < numResolutions; ++i) {
        if (devicePixelRatio <= this.availableResolutions[i].resolution) {
          this.currentResolution = this.availableResolutions[i].resolution;
          pagespeed.Responsive.updateImgSrc_(
            this.img,
            this.availableResolutions[i].url
          );
          break;
        }
      }
    }
  };
  pagespeed.Responsive.prototype.computeDevicePixelRatioWithZoom = function() {
    var zoomRatio = document.documentElement.clientWidth / window.innerWidth;
    return getPixelRatio() * zoomRatio;
  };
  pagespeed.Responsive.prototype.responsiveResize = function() {
    var devicePixelRatio = this.computeDevicePixelRatioWithZoom();
    var numImages = this.allImages_.length;
    for (var i = 0; i < numImages; ++i) {
      this.allImages_[i].responsiveResize(devicePixelRatio);
    }
  };
  pagespeed.Responsive.search_ = function(str, re) {
    var offset = str.search(re);
    if (offset == -1) {
      return str.length;
    } else {
      return offset;
    }
  };
  var WHITESPACE = /[ \t\n\f\r]/;
  var NOT_WHITESPACE = /[^ \t\n\f\r]/;
  var WHITESPACE_OR_COMMA = /[ \t\n\f\r,]/;
  var NOT_WHITESPACE_OR_COMMA = /[^ \t\n\f\r,]/;
  pagespeed.Responsive.prototype.parseSrcset = function(img, src, srcset) {
    var respImage = new pagespeed.ResponsiveImage(img);
    var has_1x = false;
    var rest = srcset;
    var pos = pagespeed.Responsive.search_(rest, NOT_WHITESPACE_OR_COMMA);
    rest = rest.slice(pos);
    while (rest.length > 0) {
      pos = pagespeed.Responsive.search_(rest, WHITESPACE);
      var url = rest.slice(0, pos);
      rest = rest.slice(pos);
      if (url[url.length - 1] == ",") {
        return null;
      }
      pos = pagespeed.Responsive.search_(rest, NOT_WHITESPACE);
      rest = rest.slice(pos);
      pos = pagespeed.Responsive.search_(rest, WHITESPACE_OR_COMMA);
      var descriptor = rest.slice(0, pos);
      rest = rest.slice(pos);
      if (descriptor.length > 1 && descriptor[descriptor.length - 1] == "x") {
        var resolution = toNumber(descriptor.slice(0, -1));
        if (isNaN(resolution)) {
          return null;
        }
        respImage.availableResolutions.push(
          new pagespeed.ResponsiveImageCandidate(resolution, url)
        );
        if (resolution == 1) {
          has_1x = true;
        }
      } else {
        return null;
      }
      pos = pagespeed.Responsive.search_(rest, NOT_WHITESPACE);
      rest = rest.slice(pos);
      if (rest.length > 0 && rest[0] != ",") {
        return null;
      } else {
        rest = rest.slice(1);
      }
      pos = pagespeed.Responsive.search_(rest, NOT_WHITESPACE_OR_COMMA);
      rest = rest.slice(pos);
    }
    if (!has_1x && src) {
      respImage.availableResolutions.push(
        new pagespeed.ResponsiveImageCandidate(1, src)
      );
    }
    respImage.availableResolutions.sort(function(a, b) {
      return a.resolution - b.resolution;
    });
    return respImage;
  };
  pagespeed.Responsive.prototype.init = function() {
    var images = document.getElementsByTagName("IMG");
    for (var i = 0, img; img = images[i]; ++i) {
      var src = img.getAttribute("src");
      var srcset = img.getAttribute("srcset");
      if (srcset) {
        var respImage = this.parseSrcset(img, src, srcset);
        if (respImage != null) {
          this.allImages_.push(respImage);
        }
      }
    }
    window.addEventListener("resize", this.responsiveResize.bind(this));
    window.addEventListener("touchmove", (event) => {
      if (event.touches.length > 1) {
        this.responsiveResize();
      }
    });
    this.responsiveResize();
  };
  pagespeed.responsiveInstance = new pagespeed.Responsive();
  pagespeed.responsiveInstance.init();
})();
