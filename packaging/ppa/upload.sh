#!/usr/bin/env bash
# Builds signed Ubuntu source packages and uploads them to your Launchpad PPA ("superv").
# Launchpad then compiles them for Ubuntu (and Linux Mint, which uses the same packages).
#
#   packaging/ppa/upload.sh <launchpad-username> [series...]
#     series default: jammy noble resolute stonking   (Ubuntu 22.04 = Mint 21, 24.04 = Mint 22, 26.04, 26.10)
#     add newer Ubuntu series by name, e.g.: upload.sh myname jammy noble <newer-series>
#
# One-time setup (see PUBLISHING.md):
#   sudo apt install devscripts dput debhelper
#   a GPG key registered on your Launchpad account
#   export DEBFULLNAME="Your Name" DEBEMAIL="you@example.com"   # must match your GPG key
# Re-uploading the same version to the same series? Bump PPA_REV: PPA_REV=2 upload.sh …
set -euo pipefail

LP_USER="${1:?usage: packaging/ppa/upload.sh <launchpad-username> [series...]}"; shift
SERIES=("$@"); ((${#SERIES[@]})) || SERIES=(jammy noble resolute stonking)
: "${DEBFULLNAME:?set DEBFULLNAME to the name on your GPG key}"
: "${DEBEMAIL:?set DEBEMAIL to the email on your GPG key}"
for tool in debuild dput git; do command -v $tool >/dev/null || { echo "missing $tool: sudo apt install devscripts dput git"; exit 1; }; done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
V="$(sed -n 's/^#define SV_VERSION *"\(.*\)"/\1/p' "$ROOT/superv.h")"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

for s in "${SERIES[@]}"; do
  ver="$V~${s}${PPA_REV:-1}"
  dir="$WORK/superv-$ver"
  echo ">> $ver for $s"
  # the committed source, without prebuilt binaries, build output or the website
  git -C "$ROOT" archive --format=tar --prefix="superv-$ver/" HEAD -- . ':!bin' ':!dist' ':!docs' | tar -x -C "$WORK"
  cp -r "$ROOT/packaging/ppa/debian" "$dir/debian"
  cat > "$dir/debian/changelog" <<EOF
superv ($ver) $s; urgency=medium

  * SuperV $V. Changes: https://github.com/saikatnjm/superv/releases/tag/v$V

 -- $DEBFULLNAME <$DEBEMAIL>  $(date -R)
EOF
  (cd "$dir" && debuild -S -d ${GPG_KEY:+-k"$GPG_KEY"})
  dput "ppa:$LP_USER/superv" "$WORK/superv_${ver}_source.changes"
done
echo ">> Uploaded. Launchpad emails you, and builds appear at https://launchpad.net/~$LP_USER/+archive/ubuntu/superv/+packages"
