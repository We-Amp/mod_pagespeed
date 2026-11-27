# shellcheck shell=bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Shared cache-volume ownership/permission logic for the container entrypoints
# (sourced by docker/entrypoint-worker.sh and docker/entrypoint-combined.sh --
# it is a library, not an executable, so it has no shebang and no `set -e`).
#
# WHY THIS EXISTS
#
# The daemon dropped from root to the unprivileged `pagespeed` user and now
# creates its cache volume and sockets 0660 / its shared config
# 0640, owned by itself.  Sharing with the web server is therefore a GROUP
# relationship, not the world-writable one the entrypoints used to arrange with
# `chmod 777` + `umask 0000` + `chmod 666`.
#
# THE CONTRACT
#
#   group `pagespeed`, GID 918, in BOTH the worker and the nginx images.
#
# The GID is FIXED and part of the image contract because the two images are
# built independently: an allocated-on-demand system GID lands on a different
# number in each image (and moves between rebuilds as the base image's package
# set changes), and two different numbers on one shared volume is exactly the
# no-sharing case.  The UID is fixed for the same reason on the other axis:
# the daemon refuses to start on cache content it does not own, so a UID that
# drifts across a rebuild turns every existing volume into a refusal.
#
# THE MIGRATION (see the "pre-existing volume" case below)
#
# A volume that predates this contract has files owned by whoever wrote them
# first -- typically root (the old root daemon) or the nginx peer's UID.  The
# daemon never chowns anything itself, by design.  This library does it in the
# one place where it is safe and bounded: the deployment's own init, as root,
# before the daemon starts, and only for the entries the daemon itself authors.

PS_CACHE_UID=918
PS_CACHE_GID=918
# Final mode for the shared data directory: setgid, owner rwx, group r-x.
#
# NOT group-writable, and NOT sticky.  Nothing on the peer path creates
# anything in here -- the serving module opens the daemon-authored volume,
# sockets and shared config and explicitly never creates the volume -- so group
# write buys nothing and costs the whole class of problems this issue is about:
# it is how the cache stem came to be owned by the nginx peer in the first
# place, and it is what lets a peer stage a symlink mid-migration.  Without
# group write the sticky bit has nothing left to protect, so it goes too.
#
# The PACKAGED host layout keeps 3770 -- that is the packaged contract and a
# separate question; this constant is the container's alone.
PS_DATA_DIR_MODE=2750
PS_CACHE_USER=pagespeed
PS_CACHE_GROUP=pagespeed

# EX_CONFIG: the container cannot start because of how it was configured//what
# is on its volume, not because of a transient fault.  Distinct from the
# daemon's own exit 1 so `docker inspect` can tell the two apart.
PS_EX_CONFIG=78

# Sidecar files the daemon authors inside its cache directory.  Mirrors
# kAuthoredSidecars in src/worker/cache_dir.cc -- keep the two in step; the
# daemon refuses to start on any of these it does not own.
_ps_authored_sidecars=(
  "pagespeed-shared.conf"
  ".pagespeed-serve-stats"
  "pagespeed-hosts.conf"
  "pagespeed.json"
  "pagespeed-webbotauth-keys.conf"
  "pagespeed-rslcap-keys.conf"
  # LEGACY: not written since 2.1 (the license apparatus was removed).
  # A volume last used by a 2.0 release may still carry them; they are adopted
  # as sidecars at their original owner-only mode and otherwise left alone.
  "pagespeed.license"
  "pagespeed.instance-id"
)

# Does THIS image carry the shared-GID contract?
#
# The release images (docker/Dockerfile.worker[.prebuilt], Dockerfile.nginx*,
# Dockerfile.combined.prebuilt) create `pagespeed` at the fixed UID/GID above.
# The in-repo test harnesses under tools/ build their own worker images from
# the base runtime and exec this same entrypoint without that identity; for
# them the contract cannot be applied and the pre-contract behaviour is kept,
# loudly.  Anything published to operators takes the first branch -- the image
# smoke test (tools/ci/test-image-cache-sharing.sh) asserts it.
ps_has_shared_identity() {
  local uid gid
  uid="$(id -u "$PS_CACHE_USER" 2>/dev/null)" || return 1
  gid="$(id -g "$PS_CACHE_USER" 2>/dev/null)" || return 1
  [ "$uid" = "$PS_CACHE_UID" ] && [ "$gid" = "$PS_CACHE_GID" ]
}

_ps_is_sidecar() {
  local name="$1" s
  for s in "${_ps_authored_sidecars[@]}"; do
    [ "$name" = "$s" ] && return 0
    [ "$name" = "${s}.tmp" ] && return 0
  done
  return 1
}

# Classify one directory entry against the configured volume stem.  Echoes one
# of: stem | gen | volume | sibling | sidecar | foreign
#
# The daemon matches its own volume files by PREFIX on the extension-free stem
# (cache_dir.cc), because the cache layer inserts -<format>-<geometry> before
# whatever extension the operator configured: "cache.vol" is authored as
# "cache-6-<16 hex>.vol" with a "cache.vol.gen" generation file beside it.
#
# The EXACT sidecar names are matched FIRST.  The prefix rule is greedy, and the
# cache path is operator-configurable: with `--cache-path /data/pagespeed.vol`
# the stem is "pagespeed", and every sidecar the daemon owns begins with that
# string.  Classified by prefix they would be reported as cache content and
# handled by the volume/sibling rules -- which fails closed, but describes the
# wrong file to the operator and applies the wrong mode.  Names the daemon
# authors under a fixed name are what they are, whatever the stem happens to be.
_ps_classify() {
  local name="$1" stem="$2" stem_noext="$3"
  if _ps_is_sidecar "$name"; then echo sidecar; return; fi
  if [ "$name" = "$stem" ]; then echo stem; return; fi
  if [ "$name" = "${stem}.gen" ]; then echo gen; return; fi
  case "$name" in
    "${stem_noext}-"*) echo volume; return ;;
    "${stem_noext}"*)  echo sibling; return ;;
  esac
  echo foreign
}

# The mode to adopt one entry AT.
#
# Adoption must never leave a file more permissive than the daemon's own writer
# would, or the migration silently widens what the group can reach: the legacy
# 2.0 license/instance-id files are owner-only secrets, and `pagespeed.json` is
# CONFIGURATION THE DAEMON APPLIES AT STARTUP -- a group-writable copy of it is
# a way to steer the daemon, not merely to read it.  Each mode below mirrors the
# call site that writes the file, and is tightened rather than widened where the
# daemon's own mode is broader than the peer actually needs (the daemon rewrites
# these files on its own terms anyway).
#
#   pagespeed.license                0600  LEGACY: 2.0 wrote it 0600; no writer
#   pagespeed.instance-id            0600  since 2.1, kept as found
#   pagespeed-shared.conf            0640  shared_config.cc:376
#   pagespeed.json                   0640  config_file.cc:691 writes 0644
#   pagespeed-hosts.conf             0640  host_aliases.cc:119 writes 0644
#   pagespeed-webbotauth-keys.conf   0640  webbotauth_warmer.cc:186 writes 0644
#   pagespeed-rslcap-keys.conf       0640  same
#   <stem>.gen                       0640  cache.cc:1364-1365 writes 0644
#   .pagespeed-serve-stats           0660  serve_stats.cc:170 (peer maps it rw)
#   volume / stem / siblings         0660  the peer opens the volume rw
#
# Where a mode here is tighter than the writer's, that is deliberate and safe
# because the peer is IN the group: the key stores are written 0644 on the
# stated grounds that "nginx workers run unprivileged and must read it" and
# that their contents are public keys (webbotauth_warmer.cc:178-184), and the
# generation file is written 0644 for the same read-access reason
# (cache.cc:1364). Group-read satisfies both without also handing them to every
# other uid on the host, and the daemon restores its own mode on first write.
# The generation file in particular is READ-ONLY to the peer, so 0660 -- which
# is what this adopted at before -- let the peer rewrite it.
# True iff the file at $1 holds nothing but a decimal generation counter --
# or nothing at all.
#
# Checked as a WHOLE FILE, deliberately. A line-oriented `grep -qE '^[0-9]*$'`
# is satisfied by any file containing a blank line, because `*` matches the
# empty line: "\n../../etc/passwd" and "HOSTILE\n6\n" both passed it.
#
# Empty and whitespace-only files are ACCEPTED: a truncated .gen is the same
# situation as the 0-byte cache stem, and the daemon reads an unparsable one as
# generation 0 and cold-starts (cache.cc:1348-1356, `f >> gen` then
# `f.fail() ? 0 : gen`). Refusing it would recreate the very outage this path
# exists to prevent.
_ps_gen_content_ok() {
  local path="$1" lines
  # Everything that is not a digit or whitespace must be nothing.
  [ -z "$(tr -d '0-9[:space:]' < "$path" 2>/dev/null)" ] || return 1
  # And there must be at most one non-blank line, so "1\n2\n" is not a counter.
  # `grep -c .` prints 0 and EXITS 1 when nothing matches, which is the empty
  # file -- an accepted case -- so its status is ignored and only its output is
  # read. An unreadable file yields no output at all and is refused.
  lines="$(grep -c . "$path" 2>/dev/null || true)"
  [ -n "$lines" ] || return 1
  [ "$lines" -le 1 ] 2>/dev/null || return 1
  return 0
}

_ps_adopt_mode() {
  local kind="$1" name="${2%.tmp}"
  if [ "$kind" = "gen" ]; then echo 0640; return; fi
  if [ "$kind" = "sidecar" ]; then
    case "$name" in
      pagespeed.license|pagespeed.instance-id) echo 0600 ;;
      .pagespeed-serve-stats)                  echo 0660 ;;
      *)                                       echo 0640 ;;
    esac
    return
  fi
  echo 0660
}

# Adopt one entry: chown to the daemon identity and set an exact mode.
#
# Refuses (returns 1) on anything that is not a lone regular file or socket -- a
# symlink or a second hard link means a chown here could hand away, or rewrite
# the mode of, bytes the cache never chose.  Same reasoning as the daemon's own
# O_NOFOLLOW + st_nlink==1 check in lib/cache/cache.cc.
#
# Two things make that check load-bearing rather than decorative:
#
#   * `chown -h` (never `chown`).  Plain chown FOLLOWS symlinks, so a check
#     that merely rejects a symlink still hands the target away if the entry
#     becomes one between the check and the call.  -h makes the syscall itself
#     incapable of dereferencing, so the worst case is a chowned symlink.
#   * the caller holds the directory at 0700 for the whole scan (see
#     ps_prepare_data_dir), so no other uid can create, rename or replace an
#     entry while these checks and calls run.  The check-then-use window is
#     closed by the directory, not by the ordering of these lines.
#
# chmod has no -h and cannot act on a symlink's own bits, so it runs only after
# the -L check, under the same directory lock, and only on entries -h chown has
# already claimed.
_ps_adopt() {
  local path="$1" mode="$2"
  if [ -L "$path" ]; then
    echo "ERROR:   $path is a SYMLINK -- refusing to chown it." >&2
    return 1
  fi
  if [ ! -f "$path" ] && [ ! -S "$path" ]; then
    echo "ERROR:   $path is neither a regular file nor a socket -- refusing to chown it." >&2
    return 1
  fi
  local nlink
  nlink="$(stat -c '%h' "$path" 2>/dev/null || echo 1)"
  if [ -f "$path" ] && [ "$nlink" != "1" ]; then
    echo "ERROR:   $path has $nlink hard links -- refusing to chown it." >&2
    return 1
  fi
  chown -h "${PS_CACHE_UID}:${PS_CACHE_GID}" "$path" || return 1
  # Re-check after the chown: if the entry became a symlink in between, -h kept
  # the syscall on the link itself, and this catches it before chmod (which
  # would follow) ever runs.
  if [ -L "$path" ]; then
    echo "ERROR:   $path became a SYMLINK during adoption -- refusing." >&2
    return 1
  fi
  chmod "$mode" "$path" || return 1
  return 0
}

# ps_prepare_data_dir <data-dir> <cache-path>
#
# Run as root, before the daemon starts.  Establishes the shared-group contract
# on the data directory, adopts a pre-existing volume where that is safe, and
# refuses -- early, loudly, with a remedy -- where it is not.
#
# PAGESPEED_ADOPT_VOLUME controls the one judgement call in here:
#   auto (default) adopt daemon-authored content left by an earlier release
#   off            never chown cache content; refuse to start and say what to do
#   1/true         same as auto (explicit form, for compose files that want it
#                  stated rather than defaulted)
ps_prepare_data_dir() {
  local dir="$1" cache_path="$2"
  local stem stem_noext adopt
  stem="$(basename "$cache_path")"
  stem_noext="${stem%.*}"
  adopt="${PAGESPEED_ADOPT_VOLUME:-auto}"

  if ! mkdir -p "$dir"; then
    echo "ERROR: cannot create the data directory $dir." >&2
    echo "ERROR: Mount a writable volume at it, or set DATA_DIR to one." >&2
    exit "$PS_EX_CONFIG"
  fi

  if ! ps_has_shared_identity; then
    # Pre-contract image (a tools/ test harness).  Reproduce the old
    # world-writable sharing rather than half-applying a contract this image
    # cannot honour -- and say so, so it is never mistaken for a release image.
    echo "NOTE: this image has no ${PS_CACHE_USER} identity at ${PS_CACHE_UID}:${PS_CACHE_GID};"
    echo "NOTE: falling back to pre-2.1 world-writable cache sharing. Development"
    echo "NOTE: images only -- the published worker/nginx images carry the group."
    if [ "$(id -u)" = "0" ]; then
      chmod 777 "$dir"
      umask 0000
      touch "$cache_path"
      chmod 666 "$cache_path"
    fi
    return 0
  fi

  if [ "$(id -u)" != "0" ]; then
    # The operator started the container as a non-root user (compose `user:`,
    # `docker run --user`).  Nothing here is possible or needed: a fresh named
    # volume inherits /data's image-time ownership, and an existing one is the
    # operator's to prepare.  Say which mode we are in -- the failure otherwise
    # looks like the daemon refusing content for no visible reason.
    echo "NOTE: container started as uid $(id -u); skipping /data ownership setup."
    if [ ! -w "$dir" ]; then
      echo "ERROR: $dir is not writable by uid $(id -u) (gid $(id -g), groups: $(id -G))." >&2
      echo "ERROR: The optimizer needs to own its cache directory. Run the container" >&2
      echo "ERROR: as uid ${PS_CACHE_UID} (group ${PS_CACHE_GID}), or prepare the volume once:" >&2
      echo "ERROR:   docker run --rm -v <volume>:/data busybox chown -R ${PS_CACHE_UID}:${PS_CACHE_GID} /data" >&2
      echo "ERROR: On Kubernetes, set securityContext.fsGroup: ${PS_CACHE_GID} on the pod." >&2
      exit "$PS_EX_CONFIG"
    fi
    # Writable is not the same as safe, and neither is writable the same as
    # SHARED.  Under an explicit `user:` the effective gid is whatever the
    # operator gave: a uid without gid 918 can run the optimizer perfectly well
    # and still create every file with a group no peer is in, so the volume
    # works and shares nothing -- in-place optimization silently off, which is
    # the failure this whole contract exists to make impossible.
    if [ "$(id -g)" != "$PS_CACHE_GID" ]; then
      echo "WARNING: =========================================================="
      echo "WARNING: this container's effective gid is $(id -g), not ${PS_CACHE_GID}."
      echo "WARNING: The optimizer will create its cache volume and sockets owned"
      echo "WARNING: by that group, and the web server -- which is a member of"
      echo "WARNING: ${PS_CACHE_GID} -- will not be able to open them. The cache will"
      echo "WARNING: work and share NOTHING: in-place optimization stays off."
      echo "WARNING: Run with gid ${PS_CACHE_GID} (compose: user: \"$(id -u):${PS_CACHE_GID}\";"
      echo "WARNING: Kubernetes: securityContext.fsGroup: ${PS_CACHE_GID})."
      echo "WARNING: =========================================================="
    fi
    # A Kubernetes emptyDir (and several CSI
    # defaults) arrive mode 0777, which makes the volume writable by every uid
    # on the node that can reach the mount -- the exact posture the group
    # contract exists to replace.  We cannot fix it from here without root, so
    # say so once, loudly, naming the setting that fixes it.
    if [ -n "$(find "$dir" -maxdepth 0 -perm -o+w 2>/dev/null)" ]; then
      echo "WARNING: =========================================================="
      echo "WARNING: $dir is WORLD-WRITABLE and this container is not root, so"
      echo "WARNING: the entrypoint cannot tighten it. Any uid that can reach"
      echo "WARNING: this mount can write into the optimizer's cache directory."
      echo "WARNING: Kubernetes: use a volume with securityContext.fsGroup:"
      echo "WARNING: ${PS_CACHE_GID} rather than a default emptyDir, or drop the"
      echo "WARNING: explicit runAsUser and let the entrypoint prepare the"
      echo "WARNING: directory as root before it drops privileges itself."
      echo "WARNING: =========================================================="
    fi
    umask 0007
    return 0
  fi

  # The directory itself.  Two steps, and the order is the security property.
  #
  # Take ownership, then LOCK THE DIRECTORY TO 0700 for the whole scan below.
  # Every adoption decision is check-then-act on a path, and a path is only as
  # stable as the directory holding it: while /data is reachable by another uid,
  # that uid can rename an entry out from under a check and put a symlink in its
  # place, and the adoption then acts on something the cache never wrote.  No
  # amount of stat-ing closes that window from inside a shell.  Removing every
  # other uid's search and write permission on the directory does close it --
  # nobody else can create, rename, or replace an entry while we work.
  #
  # The final mode is applied on the way out, on the success path only: if we
  # refuse, the directory stays 0700 and the volume stays inert rather than
  # half-migrated and reachable.
  # 2700, not 0700: chmod PRESERVES a directory's setgid bit for octal modes of
  # fewer than five digits (coreutils), so 0700 would silently leave 2700 here
  # anyway.  Say what actually happens.  The setgid bit is inert while the group
  # has no permission bits at all, and keeping it means the final mode does not
  # have to re-establish it.
  if ! chown "${PS_CACHE_UID}:${PS_CACHE_GID}" "$dir" || ! chmod 2700 "$dir"; then
    echo "ERROR: cannot take ownership of the data directory $dir." >&2
    echo "ERROR: The optimizer runs as ${PS_CACHE_USER} (uid ${PS_CACHE_UID}) and needs to own it." >&2
    echo "ERROR: Usually this means the volume is mounted read-only, or the container" >&2
    echo "ERROR: was started without the CHOWN capability (--cap-drop). Either give the" >&2
    echo "ERROR: container CHOWN for its first start, or prepare the volume once:" >&2
    echo "ERROR:   docker run --rm -v <volume>:/data busybox sh -c 'chown -R ${PS_CACHE_UID}:${PS_CACHE_GID} /data && chmod ${PS_DATA_DIR_MODE} /data'" >&2
    exit "$PS_EX_CONFIG"
  fi

  local -a refusals=()
  local -a adopted_volumes=()
  local entry path kind owner size

  for path in "$dir"/* "$dir"/.[!.]*; do
    [ -e "$path" ] || continue
    entry="$(basename "$path")"
    kind="$(_ps_classify "$entry" "$stem" "$stem_noext")"
    [ "$kind" = "foreign" ] && continue

    owner="$(stat -c '%u' "$path" 2>/dev/null || echo -1)"
    size="$(stat -c '%s' "$path" 2>/dev/null || echo -1)"

    if [ "$owner" = "$PS_CACHE_UID" ]; then
      # Already ours -- the ordinary restart path.  Re-apply the same per-entry
      # mode adoption would have used, rather than only stripping the world
      # bits: the two must not diverge, or a volume that was adopted once by an
      # older build of this script would keep whatever modes that build chose
      # and never be corrected. Setting the mode is idempotent and the daemon
      # rewrites its own files on its own terms anyway, so this costs nothing
      # and makes the end state a function of the contract rather than of which
      # version of the script first touched the volume.
      #
      # chmod follows symlinks and has no -h, so it gets the same -L guard as
      # the adoption path: an entry that is a symlink is never something to
      # relax or tighten through, whoever owns the link itself.
      if [ -L "$path" ]; then
        refusals+=("$entry (a SYMLINK owned by the daemon uid -- refusing to chmod through it)")
      else
        chmod "$(_ps_adopt_mode "$kind" "$entry")" "$path" 2>/dev/null \
          || chmod o-rwx "$path" 2>/dev/null || true
      fi
      continue
    fi

    case "$kind" in
      stem)
        # The 0-byte stem is a NAME, not content: the cache layer never writes
        # through it (the bytes live in <stem>-<format>-<geometry><ext>), and
        # the old entrypoints created it with `touch` purely so a chmod could
        # land before Cyclone opened it.  Whoever won that race owned it --
        # which is how a shared volume ends up with the peer's UID on the one
        # file that makes the daemon refuse to start.  Prove it is empty, then
        # take it: the no-migration rule is about CONTENT, and zero bytes
        # is not content.
        if [ "$size" = "0" ]; then
          if _ps_adopt "$path" "$(_ps_adopt_mode "$kind" "$entry")"; then
            echo "NOTE: adopted 0-byte cache stem $path (was uid $owner); no content was migrated."
          else
            refusals+=("$path (0-byte stem, uid $owner)")
          fi
        else
          refusals+=("$path ($size bytes, uid $owner -- not an empty stem)")
        fi
        ;;
      gen)
        # The cyclone generation counter: a decimal integer, nothing else.
        #
        # This is the ONE place where the uid-0 rule below would cost more than
        # it buys. The volume reported in #1456 has a peer-owned `.gen` -- that
        # is the actual field state this change exists to recover -- so refusing
        # a non-root `.gen` would turn the fixed outage straight back into an
        # outage. It is safe to treat differently because of what it can say:
        # the daemon reads it as a NUMBER, so the worst a hostile one can do is
        # name a generation whose volume does not exist, and the daemon cold-
        # starts. It cannot carry a path, a URL, a key or a directive, and the
        # volume it names is still subject to the uid-0 rule below.
        #
        # Content checked as a whole file, empty included -- see
        # _ps_gen_content_ok for why both of those matter.
        if [ "$size" -ge 0 ] && [ "$size" -le 32 ] \
           && [ -f "$path" ] && [ ! -L "$path" ] \
           && _ps_gen_content_ok "$path"; then
          if _ps_adopt "$path" "$(_ps_adopt_mode "$kind" "$entry")"; then
            echo "NOTE: adopted daemon-authored $path (was uid $owner)."
          else
            refusals+=("$path ($kind, uid $owner)")
          fi
        else
          refusals+=("$path ($kind, $size bytes, uid $owner -- not a plain generation counter)")
        fi
        ;;
      sibling|sidecar)
        # Staged .tmp files and the daemon's own sidecars (shared config, host
        # aliases, serve-stats, key stores, pagespeed.json, plus the legacy 2.0
        # license/instance-id files a pre-2.1 release may have left behind).
        #
        # These are NOT "harmless because they carry no cache content".  The
        # daemon READS them at startup: `pagespeed.json` is applied before the
        # command line is even parsed (main.cc, ApplyConfigJson), so it can set
        # the Web Bot Auth and RSL-CAP key directories and issuer, and the host
        # aliases.  A file the peer wrote
        # and this script adopted would therefore be configuration the peer
        # chose -- including where signature-verification keys are fetched from.
        # So the same rule as cache content applies: adopt ONLY from uid 0.
        #
        # The two documented exceptions are above: the 0-byte stem, because a
        # zero-length file is a NAME and cannot carry a directive, and the
        # generation counter, because it is checked to be nothing but digits.
        if [ "$owner" != "0" ]; then
          refusals+=("$path ($kind, uid $owner -- daemon-read state owned by a non-root uid is never adopted)")
        elif [ "$size" -ge 0 ] && [ "$size" -le 1048576 ]; then
          if _ps_adopt "$path" "$(_ps_adopt_mode "$kind" "$entry")"; then
            echo "NOTE: adopted daemon-authored $path (was uid $owner, now mode $(_ps_adopt_mode "$kind" "$entry"))."
          else
            refusals+=("$path ($kind, uid $owner)")
          fi
        else
          refusals+=("$path ($kind, $size bytes, uid $owner -- unexpectedly large)")
        fi
        ;;
      volume)
        # A real cache volume, i.e. actual content.  Adopt it ONLY when it is
        # owned by root -- that is the pre-2.1 daemon, the one identity whose
        # authorship of the cache is not in question.  Abandoning it costs a
        # full-size file of dead disk plus a cold cache, and (with
        # `depends_on: service_healthy`) the refusal takes the whole site down
        # rather than just optimization, so root-owned content is worth
        # migrating.
        #
        # A volume owned by ANY OTHER uid is refused.  In this deployment shape
        # the peer -- the web server, a uid that maps to somebody's untrusted
        # request handling -- has historically been able to create files in
        # /data, so a peer-owned volume file is not provably the old daemon's
        # cache: adopting it would launder content of unknown authorship into
        # the optimizer's own mmap. Only root, and only ever root.
        #
        # Scope of the 2700 lock this runs under: it constrains UNPRIVILEGED
        # peers, which is the threat here -- the web server's worker processes.
        # A peer running as root (an nginx MASTER sharing the volume) is not
        # constrained by any directory mode and is out of scope; a root peer on
        # the same volume is already inside the trust boundary.
        if [ "$adopt" = "off" ] || [ "$adopt" = "0" ] || [ "$adopt" = "false" ] || [ "$adopt" = "no" ]; then
          refusals+=("$path ($size bytes, uid $owner -- adoption disabled by PAGESPEED_ADOPT_VOLUME)")
        elif [ "$owner" != "0" ]; then
          refusals+=("$path ($size bytes, uid $owner -- cache content owned by a non-root uid is never adopted)")
        elif _ps_adopt "$path" 0660; then
          adopted_volumes+=("$path")
          echo "NOTE: adopted existing cache volume $path ($size bytes, was uid $owner) into ${PS_CACHE_USER}:${PS_CACHE_GROUP} mode 0660."
        else
          refusals+=("$path ($size bytes, uid $owner)")
        fi
        ;;
    esac
  done

  if [ ${#refusals[@]} -gt 0 ]; then
    echo "==============================================================================" >&2
    echo "ERROR: the optimizer cannot take ownership of its own cache directory $dir." >&2
    echo "ERROR: The daemon runs as ${PS_CACHE_USER} (uid ${PS_CACHE_UID}) and refuses to" >&2
    echo "ERROR: start on content it does not own. Offending entries:" >&2
    for entry in "${refusals[@]}"; do
      echo "ERROR:   $entry" >&2
    done
    echo "ERROR:" >&2
    echo "ERROR: Remedy -- with the containers stopped, either take ownership:" >&2
    echo "ERROR:   docker run --rm -v <volume>:/data busybox chown -R ${PS_CACHE_UID}:${PS_CACHE_GID} /data" >&2
    echo "ERROR: or discard the old cache (it is regenerable):" >&2
    echo "ERROR:   docker volume rm <volume>" >&2
    if [ "$adopt" = "off" ] || [ "$adopt" = "0" ] || [ "$adopt" = "false" ] || [ "$adopt" = "no" ]; then
      echo "ERROR: (PAGESPEED_ADOPT_VOLUME is off; unset it to let this entrypoint adopt" >&2
      echo "ERROR:  daemon-authored cache content automatically.)" >&2
    fi
    echo "ERROR:" >&2
    echo "ERROR: Failing HERE, before the daemon starts, so this message is the first" >&2
    echo "ERROR: thing in the log rather than a restart loop that never reports healthy." >&2
    echo "ERROR: (Exit code 78 = EX_CONFIG: the volume, not a transient fault.)" >&2
    echo "ERROR: $dir is left at mode 2700, so nothing else can reach the volume" >&2
    echo "ERROR: while it is in this state. Entries adopted before the refusal KEEP" >&2
    echo "ERROR: their new ownership -- the remedy below is safe to run over them." >&2
    echo "==============================================================================" >&2
    exit "$PS_EX_CONFIG"
  fi

  # Success: open the directory back up to exactly what the peer needs, which
  # is search and read, and nothing else.  See PS_DATA_DIR_MODE.
  if ! chmod "$PS_DATA_DIR_MODE" "$dir"; then
    echo "ERROR: cannot set mode $PS_DATA_DIR_MODE on $dir; refusing to start with a" >&2
    echo "ERROR: sharing boundary that cannot be vouched for." >&2
    exit "$PS_EX_CONFIG"
  fi

  if [ ${#adopted_volumes[@]} -gt 1 ]; then
    echo "NOTE: ${#adopted_volumes[@]} cache volumes are present on $dir. The daemon opens the"
    echo "NOTE: one matching its configured geometry and leaves the others in place;"
    echo "NOTE: delete the unused ones to reclaim the disk."
  fi

  # Backstop only -- every mode above is set explicitly, never inherited from
  # here.  0007 keeps the group bits the sharing contract needs and denies the
  # world, which is the whole point of replacing `umask 0000`.
  umask 0007
}

# ps_exec_worker <binary> <args...>
#
# Drop to the daemon identity and exec.  The entrypoint needs root for exactly
# one thing -- preparing/adopting /data above, which a bind mount or a
# pre-existing named volume never inherits from the image -- so the drop
# happens here rather than through a Dockerfile `USER` directive.
#
# setpriv comes from util-linux (already in the base image; no new package) and
# execs directly, so the daemon stays PID 1 and keeps receiving the signals
# docker sends.  --init-groups picks up the supplementary groups from
# /etc/group rather than leaving the process with only its primary GID.
ps_exec_worker() {
  if [ "$(id -u)" != "0" ] || ! ps_has_shared_identity; then
    exec "$@"
  fi
  echo "NOTE: dropping to ${PS_CACHE_USER} (uid ${PS_CACHE_UID}, gid ${PS_CACHE_GID}) before starting the optimizer."
  # setpriv changes the identity but not the environment, so HOME would stay
  # the launcher's (/root) -- a directory the daemon's uid cannot write.  Anything
  # the daemon spawns that keys off HOME inherits that: headless Chrome resolves
  # its crash-report database under $HOME/.config and aborts at launch when it
  # cannot create it.  Point HOME at the identity's own home (the data
  # directory) so the environment matches the uid, as `docker exec -u` would.
  local home
  home="$(getent passwd "$PS_CACHE_USER" 2>/dev/null | cut -d: -f6)"
  [ -n "$home" ] && export HOME="$home"
  # --bounding-set=-all empties the capability bounding set, so no capability
  # can be regained by any descendant.  --no-new-privs makes the drop one-way:
  # without it the base image's setuid binaries (mount, su, chsh, ...) remain
  # usable escalation targets from inside the daemon's own process tree, which
  # is most of the point of not running as root.  Neither affects
  # unshare(CLONE_NEWUSER), so Chrome's namespace sandbox is unaffected -- the
  # image gate asserts that.
  exec setpriv --reuid="$PS_CACHE_USER" --regid="$PS_CACHE_GROUP" --init-groups \
       --bounding-set=-all --no-new-privs -- "$@"
}

# ps_check_api_port -- the daemon no longer runs as root, so it cannot bind a
# privileged port.  Say so by name instead of letting it fail deep inside the
# listener setup.
ps_check_api_port() {
  local port="${1:-}"
  [ -z "$port" ] && return 0
  ps_has_shared_identity || return 0
  case "$port" in
    ''|*[!0-9]*) return 0 ;;
  esac
  if [ "$port" -lt 1024 ]; then
    echo "ERROR: PAGESPEED_API_PORT=$port is a privileged port (<1024) and the" >&2
    echo "ERROR: optimizer no longer runs as root, so it cannot bind one." >&2
    echo "ERROR: Listen on a high port and publish it instead, e.g." >&2
    echo "ERROR:   -e PAGESPEED_API_PORT=9880 -p $port:9880" >&2
    exit "$PS_EX_CONFIG"
  fi
}
