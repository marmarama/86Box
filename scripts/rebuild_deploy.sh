#!/usr/bin/env bash
# rebuild_deploy.sh - Build 86Box new_dynarec configurations and deploy atomically
set -euo pipefail

# Determine script and repository root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/CMakeLists.txt" ]]; then
    REPO_ROOT="$SCRIPT_DIR"
else
    REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
fi

DEST_DIR="${BOX86_EMU_DIR:-/home/marm/Emulation/86Box}"
DEST_BIN="$DEST_DIR/86Box"

BUILD_RELEASE=1
BUILD_DEBUG=1
DEPLOY_TARGET="release"

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
    --release-only     Only build and deploy the Release configuration
    --debug-only       Only build and deploy the Debug configuration
    --deploy-debug     Deploy the Debug binary instead of Release (builds both)
    -h, --help         Show this help message

Environment Variables:
    BOX86_EMU_DIR      Destination emulation directory (default: /home/marm/Emulation/86Box)
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release-only)
            BUILD_DEBUG=0
            DEPLOY_TARGET="release"
            shift
            ;;
        --debug-only)
            BUILD_RELEASE=0
            DEPLOY_TARGET="debug"
            shift
            ;;
        --deploy-debug)
            DEPLOY_TARGET="debug"
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "[!] Unknown option: $1" >&2
            usage
            ;;
    esac
done

NPROC=$(nproc 2>/dev/null || echo 4)

echo "============================================================"
echo " 86Box Rebuild & Redeploy"
echo " Repo Root:   $REPO_ROOT"
echo " Destination: $DEST_BIN"
echo " Deploying:   $DEPLOY_TARGET"
echo "============================================================"

# 1. Build Release
if [[ $BUILD_RELEASE -eq 1 ]]; then
    echo "[*] Building Release (build/new_dynarec) with $NPROC jobs..."
    ninja -C "$REPO_ROOT/build/new_dynarec" -j"$NPROC" 86Box
fi

# 2. Build Debug
if [[ $BUILD_DEBUG -eq 1 ]]; then
    echo "[*] Building Debug (build/new_dynarec_dev_debug) with $NPROC jobs..."
    ninja -C "$REPO_ROOT/build/new_dynarec_dev_debug" -j"$NPROC" 86Box
fi

# 3. Select source binary
if [[ "$DEPLOY_TARGET" == "release" ]]; then
    SRC_BIN="$REPO_ROOT/build/new_dynarec/src/86Box"
else
    SRC_BIN="$REPO_ROOT/build/new_dynarec_dev_debug/src/86Box"
fi

if [[ ! -f "$SRC_BIN" ]]; then
    echo "[!] Source binary not found: $SRC_BIN" >&2
    exit 1
fi

if [[ ! -d "$DEST_DIR" ]]; then
    echo "[!] Destination directory does not exist: $DEST_DIR" >&2
    exit 1
fi

# 4. Atomic deployment
echo "[*] Deploying atomically to $DEST_BIN ..."
TMP_BIN="$DEST_DIR/86Box.new.$$"
cp -f "$SRC_BIN" "$TMP_BIN"
chmod 777 "$TMP_BIN" 2>/dev/null || chmod +x "$TMP_BIN"
mv -f "$TMP_BIN" "$DEST_BIN"

BIN_SIZE=$(ls -lh "$DEST_BIN" | awk '{print $5}')
BIN_DATE=$(ls -l --time-style=+"%Y-%m-%d %H:%M:%S" "$DEST_BIN" | awk '{print $6, $7}')

echo "============================================================"
echo "[+] Successfully rebuilt and deployed $DEPLOY_TARGET binary!"
echo "    Path: $DEST_BIN ($BIN_SIZE, $BIN_DATE)"
echo "============================================================"

