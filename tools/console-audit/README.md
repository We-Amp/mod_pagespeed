# Console verification rig

A throwaway Apache + shipped mod_pagespeed + shipped pagespeed-optimizer stack
in one Ubuntu container, for checking the admin console against known numbers.

    tools/console-audit/rig.sh up          # fetch packages, boot, install, configure, warm
    tools/console-audit/rig.sh workload 3  # controlled workload, client-side expected values
    tools/console-audit/rig.sh capture     # real JSON of every console leaf
    tools/console-audit/rig.sh down

Needs `docker` (a remote `DOCKER_HOST` works; nothing is bind-mounted), `gh`
and network access to download the release packages. The containers publish
127.0.0.1 ports only. The package version defaults to the optimizer pin in
`install/debian/optimizer-pin.json`.

`workload.py` serves 6 CSS, 6 JS, 12 images and a woff2/svg pair from two
named vhosts (`site-a.test` on :80, `site-b.test` on :81) and records what the
client observed. The vhosts get different request counts, so per-host rows
differ.

To refresh the unit-test fixtures after a console or module change, bring the
rig up and run `pagespeed/system/console/scripts/refresh-stat-fixture.sh --rig`.
The tests in `src/lib/utils/rig-fixtures.test.ts` read
`pagespeed/system/console/test-fixtures/rig/`.
