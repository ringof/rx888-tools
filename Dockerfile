# rx888-tools — PPS / coherent-tone data-integrity proof kit.
#
# Contains the stream + analysis tools (librx888, pps_integrity, stream_soak,
# tone_monitor) and the offline analyzers (tone_quality.py + numpy + gnuplot).
#
# Deliberately does NOT contain chrony or pps-gpio: those discipline the host
# kernel clock and load a kernel module, so they belong on the host — see
# scripts/host-timebase-setup.sh. The hardware-free proof (synthetic
# clean/drop/garble + the gnuplot figures) runs anywhere, including amd64
# laptops, so a reviewer can see the method work without an RX888.
#
# Build (single arch):   docker build -t rx888-ppskit .
# Build (multi-arch):    docker buildx build --platform linux/arm64,linux/amd64 -t rx888-ppskit .
# Prove (no hardware):   docker run --rm -v "$PWD/out:/out" rx888-ppskit
# Live capture (Pi):     see REPRODUCE.md (adds --device for USB)

FROM debian:bookworm-slim

# System packages only — no pip, no venv. numpy and gnuplot come from apt, so
# there is no Python dependency management to contend with.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config \
        libusb-1.0-0-dev libusb-1.0-0 \
        python3 python3-numpy \
        gnuplot-nox \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/rx888-tools
COPY . .

# Build the kit and run the hardware-free proof at image-build time, so a broken
# tree fails the build. NATIVE_MARCH= keeps the build portable across CPUs (the
# default -march=native would bake in the builder's microarch); rx888_dsp is not
# built — its AVX2 path is x86-only and not part of this kit.
RUN make NATIVE_MARCH= check

# Fetch the pinned FX3 firmware blob into the image (checksum-verified) so live
# capture from the container is self-contained: tools find it at
# firmware/SDDC_FX3.img (relative to this WORKDIR). The no-hardware proof does
# not need it; live capture with a boot-mode device does.
RUN make firmware

ENV LD_LIBRARY_PATH=/opt/rx888-tools
COPY docker/reproduce.sh /usr/local/bin/reproduce
RUN chmod +x /usr/local/bin/reproduce

# Default: re-run the proof and emit the gnuplot figures to /out (if mounted).
CMD ["reproduce"]
