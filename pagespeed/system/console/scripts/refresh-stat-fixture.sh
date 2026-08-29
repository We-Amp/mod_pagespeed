#!/bin/bash
# Refresh src/lib/data/stat-names.fixture.json from a running server.
#
# The fixture is the set of statistics variable names a real server emits. The
# unit test holds every name in it to at least a family-level description, so
# the description table cannot silently rot as counters are added. When
# counters change, regenerate the fixture here and fix whatever the test then
# reports as undescribed.
#
# Usage:
#   scripts/refresh-stat-fixture.sh                    # public demo server
#   scripts/refresh-stat-fixture.sh http://host:8080/pagespeed_admin
#
# The argument is the admin console's base URL; "/statistics?format=json" is
# appended to it. Note that one server only exposes the counters its own
# module and configuration register, so a name absent here is not proof the
# counter does not exist -- it may just belong to another deployment.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$SCRIPT_DIR/../test-fixtures/stat-names.fixture.json"

BASE="${1:-https://demo-httpd-1.1.modpagespeed.com/pagespeed_global_admin}"
URL="$BASE/statistics?format=json"

for tool in curl jq; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "ERROR: $tool is required" >&2
    exit 1
  }
done

echo "Fetching $URL"
BODY="$(curl -fsS --max-time 30 "$URL")"

# .variables is the name -> value map; we keep only the sorted names.
echo "$BODY" | jq -S '.variables | keys' > "$OUT"

COUNT="$(jq 'length' < "$OUT")"
if [ "$COUNT" -lt 100 ]; then
  echo "ERROR: only $COUNT names captured; that is not a realistic dump." >&2
  exit 1
fi

echo "Wrote $COUNT names to $OUT"
echo "Now run 'npx vitest run' -- it will list any name lacking a description."
