#!/bin/bash
# Cross-build Enxerto (and the boot image) for SM6375 inside a Debian container.
# Run from the host:
#   docker run --rm -v "<repo>:/src" -e BOARD=sm6375 debian:bookworm-slim bash /src/tools/docker-build.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

echo "== installing toolchain =="
apt-get update -qq
apt-get install -y -qq gcc gcc-aarch64-linux-gnu cpio gzip findutils python3 mawk file curl >/dev/null 2>&1

# AOSP mkbootimg.py (header v0-v4) as the `mkbootimg` CLI; the script imports
# the gki/ package, so fetch the whole repo tree.
echo "== fetching mkbootimg =="
mkdir -p /opt/mkbootimg
curl -fsSL "https://android.googlesource.com/platform/system/tools/mkbootimg/+archive/refs/heads/main.tar.gz" \
  -o /tmp/mkbootimg.tgz
tar xzf /tmp/mkbootimg.tgz -C /opt/mkbootimg
ln -sf /opt/mkbootimg/mkbootimg.py /usr/local/bin/mkbootimg
chmod +x /opt/mkbootimg/mkbootimg.py

echo "== toolchain versions =="
aarch64-linux-gnu-gcc --version | head -1
head -1 /usr/local/bin/mkbootimg

BOARD=${BOARD:-sm6375}
echo "== building BOARD=$BOARD =="
cd /src
# The Windows git clone has CRLF line endings; the shebang and assembly/linker
# scripts must be LF. Strip in place (also normalizes the host copy, which is
# correct for a Unix-developed project).
sed -i 's/\r$//' enxerto/build.sh enxerto/start.S enxerto/enxerto.ld
BOARD="$BOARD" bash enxerto/build.sh
echo "== done =="
ls -l enxerto/out/Image enxerto/out/enxerto-boot.img 2>/dev/null || true
