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
- **IIS with Shared Configuration: upgrade every server together.** The
  installer writes the `PageSpeedFinalView` entry into `applicationHost.config`,
  so with a shared configuration every server sees it at once. A server still
  running an earlier version of the module loads that version for both
  entries and runs it twice per request. Upgrade all servers that share the
  configuration together, or take the servers not yet upgraded out of
  rotation until they are.
