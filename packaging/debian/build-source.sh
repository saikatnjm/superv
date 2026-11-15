#!/usr/bin/env bash
# Builds the Debian source package (for mentors.debian.net / a sponsor) in a Debian unstable container,
# unsigned, into dist/debian/. It downloads the release named in debian/changelog from GitHub and leaves out
# bin/ (see Files-Excluded in debian/copyright). Then sign and upload it on your machine:
#
#   debsign -k <your-key> dist/debian/superv_*_source.changes
#   dput mentors dist/debian/superv_*_source.changes
#
# Needs Docker. Your signing key never goes into the container.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/dist/debian"
rm -rf "$OUT" && mkdir -p "$OUT"
docker run --rm -v "$ROOT/packaging/debian:/pkg:ro" -v "$OUT:/out" -e OWNER="$(id -u):$(id -g)" debian:sid bash -ec '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends build-essential devscripts debhelper lintian pkgconf \
      libgtk-3-dev libx11-dev libwww-perl libfile-dirlist-perl libfile-touch-perl libmoo-perl \
      ca-certificates xz-utils >/dev/null
  mkdir -p /w/src && cp -r /pkg/debian /w/src/ && cd /w/src
  uscan --download-current-version --destdir /w
  mkdir /w/b && mv /w/superv_*.orig.tar.xz /w/b/ && cd /w/b && tar -xJf superv_*.orig.tar.xz
  cd "$(find /w/b -mindepth 1 -maxdepth 1 -type d)" && cp -r /pkg/debian .
  dpkg-buildpackage -S -sa -us -uc -d
  cd /w/b && lintian -I --pedantic ./*_source.changes || true
  cp /w/b/superv_* /out/ && chown -R "$OWNER" /out'
ls -l "$OUT"
