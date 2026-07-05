#!/bin/bash
# Tear down the ASan Apache rig. MUST run even on failure. Stops
# Apache and restores /etc/apache2/envvars verbatim from the setup backup (removing
# the LD_PRELOAD injection), so the next non-ASan Apache System Tests run is not
# silently ASan-preloaded. The module/config are overwritten by the next
# setup_apache_test.sh run, so only the envvars injection needs undoing here.
set -uo pipefail

REPO_DIR="${REPO_DIR:-$(pwd)}"
RIG_DIR="${RIG_DIR:-${TMPDIR:-/tmp}/apache-asan-rig}"
DOCROOT="${DOCROOT:-/var/www/html}"
ENVV=/etc/apache2/envvars

# 1. Stop Apache (best-effort). We started it directly (not systemd), so kill the
#    processes and clear any failed systemd unit state for runner hygiene.
sudo bash -c "set -a; source $ENVV; apache2 -k graceful-stop" 2>/dev/null || true
sudo pkill -9 -x apache2 2>/dev/null || true
# Undo the setup mask so the runner's systemd apache works for the next job.
sudo systemctl unmask apache2 2>/dev/null || true
sudo systemctl reset-failed apache2 2>/dev/null || true

# 2. Restore envvars: prefer the verbatim backup, else strip the marker block.
if [[ -f "$RIG_DIR/envvars.bak" ]]; then
  sudo cp "$RIG_DIR/envvars.bak" "$ENVV"
  echo "restored $ENVV from backup"
else
  sudo sed -i '/# ASAN-STRESS-BEGIN/,/# ASAN-STRESS-END/d' "$ENVV"
  echo "stripped ASan marker block from $ENVV"
fi
if sudo grep -q 'ASAN-STRESS-BEGIN' "$ENVV" 2>/dev/null; then
  echo "WARNING: our ASan injection marker still present in $ENVV — check manually"; exit 1
fi

# 3. Remove the rig vhost + corpus; restore ports.conf + re-enable the default site.
sudo a2dissite zz-apache-asan-rig >/dev/null 2>&1 || true
sudo rm -f /etc/apache2/sites-available/zz-apache-asan-rig.conf 2>/dev/null || true
[[ -f "$RIG_DIR/ports.conf.bak" ]] && sudo cp "$RIG_DIR/ports.conf.bak" /etc/apache2/ports.conf
sudo a2ensite 000-default >/dev/null 2>&1 || true
sudo rm -rf "$DOCROOT/stress" 2>/dev/null || true
echo "CLEANUP DONE (envvars clean, apache stopped, rig vhost removed)"
