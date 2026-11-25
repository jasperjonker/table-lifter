#!/usr/bin/env bash
set -euo pipefail
mkdir -p /home/vscode/.local/share/table-lifter
if [ ! -f /home/vscode/.local/share/table-lifter/signing.pem ]; then
    umask 077
    openssl ecparam -name prime256v1 -genkey -noout \
        -out /home/vscode/.local/share/table-lifter/signing.pem
fi
if [ ! -d .west ]; then
    west init -l app
fi
west update
python3 -m pip install -r zephyr/scripts/requirements.txt
west zephyr-export
bash scripts/apply-zephyr-patches.sh
