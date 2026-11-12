#!/usr/bin/env bash
# Point the AUR files at a released version: updates pkgver, the checksum and .SRCINFO.
# Works on any Linux (no makepkg needed). Run after the GitHub release vVERSION exists.
#   packaging/arch/bump.sh 2.0.0   (keep pkgver at the last release until then: the checksum must match)
set -euo pipefail
V="${1:?usage: packaging/arch/bump.sh <version, e.g. 1.2.3>}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL="https://github.com/saikatnjm/superv/archive/refs/tags/v$V.tar.gz"
SUM="$(curl -fsSL "$URL" | sha256sum | cut -d' ' -f1)"
sed -i -e "s/^pkgver=.*/pkgver=$V/" -e "s/^pkgrel=.*/pkgrel=1/" \
       -e "s/^sha256sums=.*/sha256sums=('$SUM')/" "$DIR/PKGBUILD"
sed -i -e "s/^\tpkgver = .*/\tpkgver = $V/" -e "s/^\tpkgrel = .*/\tpkgrel = 1/" \
       -e "s|^\tsource = .*|\tsource = superv-$V.tar.gz::$URL|" \
       -e "s/^\tsha256sums = .*/\tsha256sums = $SUM/" "$DIR/.SRCINFO"
echo "PKGBUILD and .SRCINFO now point at v$V ($SUM)"
