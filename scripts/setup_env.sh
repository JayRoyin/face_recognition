#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# setup_env.sh — single-source entry point for the whole project.
#
# Idempotent. Safe to source from any directory.
#
# Usage:
#     source scripts/setup_env.sh
#
# What it does:
#     1. Sources install/ros2/setup.bash (ROS2 Humble colcon overlay), so all
#        face_recognition_* packages + dependencies are visible to ros2 CLI.
#        NOTE: the ROS2 install space lives under install/ros2/ so it does not
#        get mixed with the plain-CMake artifacts in install/{bin,lib,include}.
#     2. Prepends install/vendored/lib to LD_LIBRARY_PATH so:
#          - face_recognition_* binaries load our vendored libsqlite3.so first
#          - libspatialite.so.7 (used by compressed_image_transport,
#            theora_image_transport, etc.) finds sqlite3_rtree_query_callback
#            in the vendored libsqlite3 instead of crashing with
#            "undefined symbol".
#     3. Optionally picks up FACE_DETECTION_MODEL / FACE_RECOGNITION_MODEL
#        env vars if set (used by face_db_web at enrollment time).
# -----------------------------------------------------------------------------
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VENDORED_LIB="$PROJECT_ROOT/install/vendored/lib"
ROS2_INSTALL="$PROJECT_ROOT/install/ros2"

# 1) ROS2 overlay ----------------------------------------------------------
if [ -f "$ROS2_INSTALL/setup.bash" ]; then
    if [ -z "${COLCON_CURRENT_PREFIX:-}" ] || \
       [ "${COLCON_CURRENT_PREFIX%/}" != "${ROS2_INSTALL%/}" ]; then
        # shellcheck disable=SC1091
        source "$ROS2_INSTALL/setup.bash"
    fi
else
    echo "[setup_env] WARN: $ROS2_INSTALL/setup.bash not found."
    echo "[setup_env] Did you run './build.sh ROS2' (or ALL)?"
fi

# 2) Vendored third-party libs --------------------------------------------
if [ -d "$VENDORED_LIB" ]; then
    # Remove any prior occurrence of this path, then prepend it.
    LD_LIBRARY_PATH="$(echo "${LD_LIBRARY_PATH:-}" | tr ':' '\n' \
        | grep -v "^${VENDORED_LIB}$" | paste -sd:)"
    export LD_LIBRARY_PATH="$VENDORED_LIB:${LD_LIBRARY_PATH:-}"
    if [ -d "$VENDORED_LIB/pkgconfig" ]; then
        PKG_CONFIG_PATH="$(echo "${PKG_CONFIG_PATH:-}" | tr ':' '\n' \
            | grep -v "^${VENDORED_LIB}/pkgconfig$" | paste -sd:)"
        export PKG_CONFIG_PATH="$VENDORED_LIB/pkgconfig:${PKG_CONFIG_PATH:-}"
    fi
else
    echo "[setup_env] WARN: $VENDORED_LIB does not exist."
    echo "[setup_env] Run './build.sh ROS2' (or ALL) to build vendored libs."
fi

# 3) Convenience defaults for face_db_web ---------------------------------
# If the user has models/ with the standard names, point the env vars at
# them so face_db_web auto-loads without CLI flags.
if [ -z "${FACE_DETECTION_MODEL:-}" ] && [ -f "$PROJECT_ROOT/models/det_10g.onnx" ]; then
    export FACE_DETECTION_MODEL="$PROJECT_ROOT/models/det_10g.onnx"
fi
if [ -z "${FACE_RECOGNITION_MODEL:-}" ] && [ -f "$PROJECT_ROOT/models/w600k_r50.onnx" ]; then
    export FACE_RECOGNITION_MODEL="$PROJECT_ROOT/models/w600k_r50.onnx"
fi

echo "[setup_env] vendored LD_LIBRARY_PATH pinned to:"
echo "           $VENDORED_LIB"
echo "[setup_env] verify:"
echo "           ros2 run usb_cam usb_cam_node_exe --help   # no spatialite error?"
echo "           sqlite3 :memory: 'PRAGMA compile_options;' | grep -i rtree"