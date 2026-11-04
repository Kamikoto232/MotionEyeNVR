#!/bin/bash
# Run inside the Debian 13 build environment after ./build.sh (needs dpkg-dev for dependency detection).
# Produces dist/motioneye-nvr_<version>_amd64.deb
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(grep -oP 'project\(.*VERSION \K[0-9.]+' CMakeLists.txt)
PKG=build/pkg
rm -rf "$PKG" && mkdir -p "$PKG/DEBIAN" "$PKG/usr/bin" "$PKG/etc/xdg/autostart" "$PKG/usr/share/applications"

install -m 755 build/motioneye-nvr.stripped "$PKG/usr/bin/motioneye-nvr"
install -m 644 packaging/motioneye-nvr-autostart.desktop "$PKG/etc/xdg/autostart/motioneye-nvr.desktop"
install -m 644 packaging/motioneye-nvr.desktop "$PKG/usr/share/applications/motioneye-nvr.desktop"

# shared-library dependencies detected from the binary itself
mkdir -p build/debian-tmp/debian && touch build/debian-tmp/debian/control
SHLIBS=$(cd build/debian-tmp && dpkg-shlibdeps -O -e"../pkg/usr/bin/motioneye-nvr" 2>/dev/null | sed 's/^shlibs:Depends=//')

cat > "$PKG/DEBIAN/control" <<EOF
Package: motioneye-nvr
Version: $VERSION
Section: video
Priority: optional
Architecture: amd64
Depends: $SHLIBS
Maintainer: local <root@localhost>
Description: Camera mosaic and recordings player for motionEye
 Shows all cameras of one or more motionEye servers (local or remote) as a mosaic,
 starts automatically with the graphical session and plays the recordings per day.
EOF

mkdir -p dist
dpkg-deb --root-owner-group --build "$PKG" "dist/motioneye-nvr_${VERSION}_amd64.deb"
dpkg-deb -I "dist/motioneye-nvr_${VERSION}_amd64.deb" | sed -n 1,20p
