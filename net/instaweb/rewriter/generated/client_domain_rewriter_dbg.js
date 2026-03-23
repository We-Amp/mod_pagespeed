(function(){var ENTER_KEY_CODE = 13;
window.pagespeed = window.pagespeed || {};
var pagespeed = window.pagespeed;
pagespeed.ClientDomainRewriter = function(a) {
  this.mappedDomainNames_ = a;
};
pagespeed.ClientDomainRewriter.prototype.anchorListener = function(a) {
  a = a || window.event;
  if (a.type != "keypress" || a.keyCode == ENTER_KEY_CODE) {
    for (var b = a.target; b != null; b = b.parentNode) {
      if (b.tagName == "A") {
        this.processEvent(b.href, a);
        break;
      }
    }
  }
};
pagespeed.ClientDomainRewriter.prototype.addEventListeners = function() {
  var a = this;
  document.body.onclick = function(b) {
    a.anchorListener(b);
  };
  document.body.onkeypress = function(b) {
    a.anchorListener(b);
  };
};
pagespeed.ClientDomainRewriter.prototype.processEvent = function(a, b) {
  for (var c = 0; c < this.mappedDomainNames_.length; c++) {
    if (a.indexOf(this.mappedDomainNames_[c]) == 0) {
      window.location = window.location.protocol + "//" + window.location.hostname + "/" + a.substr(this.mappedDomainNames_[c].length);
      b.preventDefault();
      break;
    }
  }
};
pagespeed.clientDomainRewriterInit = function(a) {
  a = new pagespeed.ClientDomainRewriter(a);
  pagespeed.clientDomainRewriter = a;
  a.addEventListeners();
};
pagespeed.clientDomainRewriterInit = pagespeed.clientDomainRewriterInit;
})();
