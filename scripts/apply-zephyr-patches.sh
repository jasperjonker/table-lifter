#!/usr/bin/env bash
set -euo pipefail
patch=app/patches/vl53l1x-range-status.patch
if git -C zephyr apply --reverse --check "../$patch" 2>/dev/null; then
    exit 0
fi
git -C zephyr apply --check "../$patch"
git -C zephyr apply "../$patch"
