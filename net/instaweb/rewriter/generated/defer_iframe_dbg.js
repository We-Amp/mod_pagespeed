(function(){window.pagespeed = window.pagespeed || {};
var pagespeed = window.pagespeed;
pagespeed.DeferIframe = function() {
};
pagespeed.DeferIframe.prototype.convertToIframe = function() {
  var a = document.getElementsByTagName("pagespeed_iframe");
  if (a.length > 0) {
    a = a[0];
    for (var d = document.createElement("iframe"), b = 0, c = a.attributes, e = c.length; b < e; ++b) {
      d.setAttribute(c[b].name, c[b].value);
    }
    a.parentNode.replaceChild(d, a);
  }
};
pagespeed.DeferIframe.prototype.convertToIframe = pagespeed.DeferIframe.prototype.convertToIframe;
pagespeed.deferIframeInit = function() {
  var a = new pagespeed.DeferIframe();
  pagespeed.deferIframe = a;
};
pagespeed.deferIframeInit = pagespeed.deferIframeInit;
})();
