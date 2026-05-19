#!/usr/bin/env bash
# Bootstrap script run at container start by docker-compose.dev.yml.
# Installs librx888 dev files, then builds gr-rx888 if the repo is mounted.
# After setup, drops into an interactive bash shell.
set -euo pipefail

RXTOOLS=/workspace/rx888-tools
GRBLK=/workspace/gr-rx888

echo "=== [rx888-tools] building librx888 ==="
make -C "$RXTOOLS" install-dev PREFIX=/usr/local DESTDIR="" -j"$(nproc)"
ldconfig

if [ -d "$GRBLK" ]; then
    echo ""
    echo "=== [gr-rx888] configuring ==="
    cmake -S "$GRBLK" -B "$GRBLK/build" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_INSTALL_PREFIX=/usr/local

    echo "=== [gr-rx888] building ==="
    make -C "$GRBLK/build" -j"$(nproc)"

    echo "=== [gr-rx888] installing ==="
    make -C "$GRBLK/build" install
    ldconfig

    echo ""
    echo "gr-rx888 installed.  Try:"
    echo "  gnuradio-companion  (if X11 forwarding is active)"
    echo "  python3 $GRBLK/examples/synth_pps_test.py"
else
    echo ""
    echo "gr-rx888 not mounted at $GRBLK — skipping."
    echo "Mount with: docker compose -f docker-compose.dev.yml up"
fi

echo ""
echo "librx888 pkg-config entry:"
pkg-config --modversion librx888 && pkg-config --libs librx888

echo ""
echo "=== bootstrap complete — dropping into shell ==="
exec bash
