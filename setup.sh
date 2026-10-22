#!/usr/bin/env bash
set -euo pipefail

MODULE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [ -f "$MODULE_DIR/module.conf" ]; then
  # shellcheck disable=SC1091
  source "$MODULE_DIR/module.conf"
fi

log() { printf '[ABK module] %s\n' "$*"; }
die() { printf '[ABK module][error] %s\n' "$*" >&2; exit 1; }

for name in KERNEL_ROOT DEFCONFIG CUSTOM_EXTERNAL_MODULE_STAGE; do
  if [ -z "${!name:-}" ]; then
    die "required environment variable is empty: $name"
  fi
done

command -v python3 >/dev/null 2>&1 || die "python3 is required by this module"

log "module: ${ABK_MODULE_NAME:-ABK external module}"
log "version: ${ABK_MODULE_VERSION:-unknown}"
log "stage: $CUSTOM_EXTERNAL_MODULE_STAGE"
log "kernel root: $KERNEL_ROOT"
log "defconfig: $DEFCONFIG"

exec python3 "$MODULE_DIR/scripts/abk_dsu_avb.py" \
  --stage "$CUSTOM_EXTERNAL_MODULE_STAGE" \
  --kernel-root "$KERNEL_ROOT" \
  --defconfig "$DEFCONFIG" \
  --module-dir "$MODULE_DIR"
