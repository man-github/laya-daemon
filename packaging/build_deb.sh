#!/bin/bash
set -euo pipefail

# -----------------------------------------------------------------------------
# Laya AI Daemon - Automated Debian (.deb) Release Packager
# -----------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="1.0.0"
PACKAGE_NAME="laya-daemon"
ARCH="amd64"
DEB_FILE="${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
DIST_DIR="$REPO_ROOT/dist"
STAGING_DIR="$DIST_DIR/staging_${PACKAGE_NAME}"

echo "=== Building Laya AI Debian Package: ${DEB_FILE} ==="
echo "Repository Root: $REPO_ROOT"
echo "Dist Directory:  $DIST_DIR"

# 1. Compile Binaries
echo "[1/5] Compiling production release binaries..."
cd "$REPO_ROOT"
make clean
make -j"$(nproc)"

# 2. Setup Staging Directory
echo "[2/5] Constructing Debian filesystem staging tree..."
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/DEBIAN"
mkdir -p "$STAGING_DIR/usr/bin"
mkdir -p "$STAGING_DIR/etc/laya"
mkdir -p "$STAGING_DIR/lib/systemd/system"
mkdir -p "$STAGING_DIR/var/log/laya"
mkdir -p "$STAGING_DIR/run/laya"
mkdir -p "$STAGING_DIR/usr/share/doc/$PACKAGE_NAME"

# 3. Copy Binaries
echo "[3/5] Installing binaries and configuration files..."
install -m 0755 "$REPO_ROOT/build/laya_daemon" "$STAGING_DIR/usr/bin/laya_daemon"
install -m 0755 "$REPO_ROOT/build/laya_client" "$STAGING_DIR/usr/bin/laya_client"
install -m 0755 "$REPO_ROOT/build/benchmark_client" "$STAGING_DIR/usr/bin/benchmark_client"
install -m 0755 "$REPO_ROOT/build/benchmark_per_core" "$STAGING_DIR/usr/bin/benchmark_per_core"

# Copy Configuration
install -m 0644 "$REPO_ROOT/config/laya.xml" "$STAGING_DIR/etc/laya/laya.xml"
install -m 0644 "$REPO_ROOT/config/laya_section.xml" "$STAGING_DIR/etc/laya/laya_section.xml"

# Copy Systemd Unit
install -m 0644 "$REPO_ROOT/systemd/laya-daemon.service" "$STAGING_DIR/lib/systemd/system/laya-daemon.service"

# Copy Documentation
install -m 0644 "$REPO_ROOT/README.md" "$STAGING_DIR/usr/share/doc/$PACKAGE_NAME/README.md"
if [ -f "$REPO_ROOT/LICENSE" ]; then
    install -m 0644 "$REPO_ROOT/LICENSE" "$STAGING_DIR/usr/share/doc/$PACKAGE_NAME/copyright"
fi

# 4. Copy Debian Metadata & Scripts
echo "[4/5] Configuring Debian package metadata..."
cp "$REPO_ROOT/debian/control" "$STAGING_DIR/DEBIAN/control"
cp "$REPO_ROOT/debian/conffiles" "$STAGING_DIR/DEBIAN/conffiles"
cp "$REPO_ROOT/debian/postinst" "$STAGING_DIR/DEBIAN/postinst"
cp "$REPO_ROOT/debian/prerm" "$STAGING_DIR/DEBIAN/prerm"
cp "$REPO_ROOT/debian/postrm" "$STAGING_DIR/DEBIAN/postrm"

chmod 0755 "$STAGING_DIR/DEBIAN/postinst"
chmod 0755 "$STAGING_DIR/DEBIAN/prerm"
chmod 0755 "$STAGING_DIR/DEBIAN/postrm"
chmod 0644 "$STAGING_DIR/DEBIAN/control"
chmod 0644 "$STAGING_DIR/DEBIAN/conffiles"

# 5. Build Debian Package
echo "[5/5] Executing dpkg-deb to produce package..."
mkdir -p "$DIST_DIR"
dpkg-deb --build --root-owner-group "$STAGING_DIR" "$DIST_DIR/$DEB_FILE"

echo "=== Package Build Success ==="
echo "Artifact: $DIST_DIR/$DEB_FILE"
echo ""
echo "=== Package Metadata ==="
dpkg-deb -I "$DIST_DIR/$DEB_FILE"
echo ""
echo "=== Package File Listing ==="
dpkg-deb -c "$DIST_DIR/$DEB_FILE"
