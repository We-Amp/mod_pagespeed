- **IIS: the installer adds a second module entry, `PageSpeedFinalView`.** It
  loads the same `pagespeed_iis.dll` and only reads the response as IIS sends
  it; the HTML cache needs it to see headers that IIS adds after the module
  has run, such as `customHeaders`. The installer registers it as a global
  module and puts it first in `system.webServer/modules`; an upgrade and an
  uninstall handle both entries. If you register the module by hand, add it
  the same way, first in the list:
  `appcmd install module /name:PageSpeedFinalView /image:<path to pagespeed_iis.dll> /add:false`
  then
  `appcmd set config -section:system.webServer/modules "/+[@start,name='PageSpeedFinalView']" /commit:apphost`.
  A site that removes the entry from its module list keeps working; its HTML
  cache report then reads only the headers the module received, and the
  module logs that once.
