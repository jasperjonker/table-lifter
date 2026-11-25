FROM ubuntu:26.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build gperf ccache dfu-util device-tree-compiler \
    gcc-arm-none-eabi binutils-arm-none-eabi gdb-multiarch \
    wget git openssl python3 python3-pip python3-venv python3-dev xz-utils file \
    libsdl2-dev libmagic1 ca-certificates && rm -rf /var/lib/apt/lists/*
RUN usermod -l vscode -d /home/vscode -m ubuntu && groupmod -n vscode ubuntu && usermod -s /bin/bash vscode
RUN mkdir -p /home/vscode/.local/share/table-lifter && chown -R vscode:vscode /home/vscode/.local
USER vscode
RUN python3 -m venv /home/vscode/.venv && /home/vscode/.venv/bin/pip install --no-cache-dir west
ENV PATH=/home/vscode/.venv/bin:/opt/SEGGER/JLink:$PATH
ENV ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
ENV GNUARMEMB_TOOLCHAIN_PATH=/usr
WORKDIR /workspaces/table-lifter
