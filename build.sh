#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
SRC_DIR="$PROJECT_ROOT/src"
CORE_DIR="$SRC_DIR/face_recognition_core"
ROS1_DIR="$SRC_DIR/face_recognition_ros1"
ROS2_DIR="$SRC_DIR/face_recognition_ros2"
WEB_DIR="$SRC_DIR/face_db_web"
STANDALONE_DIR="$SRC_DIR/face_recognition_standalone"
TP_DIR="$PROJECT_ROOT/third_party"
TP_SQLITE_DIR="$TP_DIR/sqlite3"
TP_SPATIALITE_DIR="$TP_DIR/spatialite"

# Vendored SQLite prefix — exported so consumer CMakeLists can pick it up.
export FACE_RECOGNITION_VENDORED_PREFIX="$PROJECT_ROOT/install/vendored"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

BUILD_LOG="$PROJECT_ROOT/build.log"

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [INFO] $1" >> "$BUILD_LOG"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [SUCCESS] $1" >> "$BUILD_LOG"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [WARN] $1" >> "$BUILD_LOG"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [ERROR] $1" >> "$BUILD_LOG"
}

print_usage() {
    cat << EOF
Usage: $0 [option]

Options:
    ROS1        Build ROS1 (Noetic) version
    ROS2        Build ROS2 (Humble) version
    CORE        Build core algorithm library
    WEB         Build web management interface
    STANDALONE  Build standalone (no ROS) C++ real-time recognition app
    MODELS      Download face recognition models
    ALL         Build all modules (default)
    CLEAN       Clean build artifacts
    HELP        Show this help

Examples:
    $0 ROS2          # Build ROS2 version
    $0 CORE          # Build core library
    $0 STANDALONE    # Build standalone (non-ROS) C++ app
    $0 MODELS        # Download models
    $0 ALL           # Build everything
    $0 CLEAN         # Clean build artifacts

EOF
}

check_dependencies() {
    local target="${1:-ALL}"
    log_info "Checking system dependencies (target=$target)..."

    local missing_deps=()

    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi

    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi

    if ! pkg-config --exists opencv4 2>/dev/null && ! pkg-config --exists opencv &>/dev/null; then
        if ! ldconfig -p | grep -q libopencv; then
            missing_deps+=("libopencv-dev")
        fi
    fi

    if ! ldconfig -p | grep -q libsqlite3; then
        log_warn "System libsqlite3 not found — vendored build will be used"
    fi

    if ! ldconfig -p | grep -q libuuid; then
        missing_deps+=("uuid-dev")
    fi

    # Web target additionally needs libmicrohttpd.
    if [ "$target" = "WEB" ] || [ "$target" = "ALL" ]; then
        if ! ldconfig -p | grep -q libmicrohttpd; then
            missing_deps+=("libmicrohttpd-dev  # required by WEB")
        fi
    fi

    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_warn "Missing dependencies: ${missing_deps[*]}"
        log_info "Install: sudo apt-get install ${missing_deps[*]}"
        return 1
    fi

    log_success "System dependency check passed"
    return 0
}

check_ros_environment() {
    local ros_version="$1"

    if [ "$ros_version" = "ROS1" ]; then
        if [ -f "/opt/ros/noetic/setup.bash" ]; then
            log_success "ROS1 (Noetic) installed"
            return 0
        else
            log_warn "ROS1 (Noetic) not installed"
            return 1
        fi
    elif [ "$ros_version" = "ROS2" ]; then
        if [ -f "/opt/ros/humble/setup.bash" ]; then
            log_success "ROS2 (Humble) installed"
            return 0
        else
            log_warn "ROS2 (Humble) not installed"
            return 1
        fi
    fi
    return 0
}

download_models() {
    log_info "=========================================="
    log_info "Downloading face recognition models"
    log_info "=========================================="

    export all_proxy="${all_proxy:-http://127.0.0.1:7890}"

    local models_dir="$PROJECT_ROOT/models"
    mkdir -p "$models_dir"

    if ! command -v python3 &> /dev/null; then
        log_error "Python3 not installed, cannot run download script"
        return 1
    fi

    local script_path="$PROJECT_ROOT/scripts/download_models.py"
    if [ ! -f "$script_path" ]; then
        log_error "Download script not found: $script_path"
        return 1
    fi

    log_info "Checking Python syntax..."
    if ! python3 -m py_compile "$script_path" 2>&1; then
        log_error "Python script has syntax errors"
        return 1
    fi
    log_success "Python syntax check passed"

    log_info "Running download script..."
    python3 "$script_path" --output-dir "$models_dir" >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Download failed, see log: $BUILD_LOG"
        return 1
    fi

    local detection_model="$models_dir/det_10g.onnx"
    if [ -f "$detection_model" ] && [ $(stat -c%s "$detection_model") -gt 1000000 ]; then
        log_success "RetinaFace model: $detection_model ($(stat -c%s "$detection_model") bytes)"
    else
        log_warn "RetinaFace model not found or too small"
    fi

    local arcface_model="$models_dir/w600k_r50.onnx"
    if [ -f "$arcface_model" ] && [ $(stat -c%s "$arcface_model") -gt 10000000 ]; then
        log_success "ArcFace model: $arcface_model ($(stat -c%s "$arcface_model") bytes)"
    else
        log_warn "ArcFace model not found or too small"
    fi

    log_success "Models download complete"
    return 0
}

clean_build() {
    log_info "Cleaning build artifacts..."

    local dirs=(
        "$PROJECT_ROOT/build"
        "$PROJECT_ROOT/devel"
        "$PROJECT_ROOT/install"
        "$PROJECT_ROOT/log"
        "$CORE_DIR/build"
        "$CORE_DIR/install"
        "$CORE_DIR/log"
        "$ROS1_DIR/build"
        "$ROS1_DIR/install"
        "$ROS1_DIR/log"
        "$ROS2_DIR/build"
        "$ROS2_DIR/install"
        "$ROS2_DIR/log"
        "$WEB_DIR/build"
        "$STANDALONE_DIR/build"
    )

    for dir in "${dirs[@]}"; do
        if [ -d "$dir" ]; then
            rm -rf "$dir"
            log_info "Removed: $dir"
        fi
    done

    find "$SRC_DIR" -type d -name "build" -exec rm -rf {} + 2>/dev/null || true
    find "$SRC_DIR" -type d -name "install" -exec rm -rf {} + 2>/dev/null || true
    find "$SRC_DIR" -type d -name "log" -exec rm -rf {} + 2>/dev/null || true

    rm -f "$BUILD_LOG"
    log_success "Clean complete"
}

# -----------------------------------------------------------------------------
# Vendored SQLite3 — built with SQLITE_ENABLE_RTREE + GEOPOLY so any extension
# (SpatiaLite, GDAL rtree driver, etc.) loads cleanly on any platform.
# -----------------------------------------------------------------------------
build_vendored_sqlite() {
    log_info "=========================================="
    log_info "Building vendored SQLite3 (third_party/sqlite3)"
    log_info "=========================================="

    if [ ! -d "$TP_SQLITE_DIR" ]; then
        log_error "Missing directory: $TP_SQLITE_DIR"
        return 1
    fi

    # Auto-fetch amalgamation if needed (skipped if already present).
    if [ ! -f "$TP_SQLITE_DIR/sqlite3.c" ] || [ ! -f "$TP_SQLITE_DIR/sqlite3.h" ]; then
        if [ -x "$TP_SQLITE_DIR/download.sh" ]; then
            log_info "Fetching SQLite amalgamation via download.sh ..."
            (cd "$TP_SQLITE_DIR" && ./download.sh) >> "$BUILD_LOG" 2>&1 || {
                log_error "download.sh failed — see log"
                return 1
            }
        else
            log_error "sqlite3.c/sqlite3.h missing and download.sh not executable"
            return 1
        fi
    fi

    local prefix="$FACE_RECOGNITION_VENDORED_PREFIX"
    local build_dir="$TP_SQLITE_DIR/build"
    mkdir -p "$build_dir"

    cmake -S "$TP_SQLITE_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configure of vendored SQLite failed, see log"
        return 1
    fi

    cmake --build "$build_dir" -j"$(nproc)" >> "$BUILD_LOG" 2>&1
    if [ $? -ne 0 ]; then
        log_error "Compile of vendored SQLite failed, see log"
        return 1
    fi

    cmake --install "$build_dir" >> "$BUILD_LOG" 2>&1
    if [ $? -ne 0 ]; then
        log_error "Install of vendored SQLite failed, see log"
        return 1
    fi

    log_success "Vendored SQLite3 installed at $prefix"
    return 0
}

# -----------------------------------------------------------------------------
# Opt-in vendored SpatiaLite. Off by default; opt in with:
#     BUILD_SPATIALITE=ON ./build.sh ...
# -----------------------------------------------------------------------------
build_vendored_spatialite() {
    log_info "=========================================="
    log_info "Building vendored SpatiaLite (third_party/spatialite)"
    log_info "=========================================="

    if [ ! -d "$TP_SPATIALITE_DIR" ]; then
        log_error "Missing directory: $TP_SPATIALITE_DIR"
        return 1
    fi

    # Auto-fetch SpatiaLite source if absent.
    if [ ! -d "$TP_SPATIALITE_DIR/src/libspatialite" ]; then
        if [ -x "$TP_SPATIALITE_DIR/download.sh" ]; then
            log_info "Fetching SpatiaLite source via download.sh ..."
            (cd "$TP_SPATIALITE_DIR" && ./download.sh) >> "$BUILD_LOG" 2>&1 || {
                log_error "download.sh failed — see log"
                return 1
            }
        else
            log_error "SpatiaLite source missing and download.sh not executable"
            return 1
        fi
    fi

    local prefix="$FACE_RECOGNITION_VENDORED_PREFIX"
    local build_dir="$TP_SPATIALITE_DIR/build"
    mkdir -p "$build_dir"

    # GEOS/PROJ headers come from the system; sqlite3 from the vendored build.
    cmake -S "$TP_SPATIALITE_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DBUILD_VENDORED_SPATIALITE=ON \
        -DCMAKE_PREFIX_PATH="$prefix" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configure of vendored SpatiaLite failed, see log"
        return 1
    fi

    cmake --build "$build_dir" -j"$(nproc)" >> "$BUILD_LOG" 2>&1
    if [ $? -ne 0 ]; then
        log_error "Compile of vendored SpatiaLite failed, see log"
        return 1
    fi

    cmake --install "$build_dir" >> "$BUILD_LOG" 2>&1
    if [ $? -ne 0 ]; then
        log_error "Install of vendored SpatiaLite failed, see log"
        return 1
    fi

    log_success "Vendored SpatiaLite installed at $prefix"
    return 0
}

# -----------------------------------------------------------------------------
# Always link/run against the vendored sqlite3 (and spatialite if built).
# -----------------------------------------------------------------------------
setup_vendored_rpath() {
    local prefix="$FACE_RECOGNITION_VENDORED_PREFIX"
    local libdir="$prefix/lib"
    if [ -d "$libdir" ]; then
        export LD_LIBRARY_PATH="$libdir:${LD_LIBRARY_PATH:-}"
        # For pkg-config consumers that don't go through our CMake hook:
        export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
        log_info "Vendored lib path pinned: $libdir"
    fi
}

build_core() {
    log_info "=========================================="
    log_info "Building core library (face_recognition_core)"
    log_info "=========================================="

    if [ ! -d "$CORE_DIR" ]; then
        log_error "Core directory not found: $CORE_DIR"
        return 1
    fi

    local build_dir="$CORE_DIR/build"
    mkdir -p "$build_dir"

    cd "$build_dir"

    cmake "$CORE_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PROJECT_ROOT/install" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configuration failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Compiling..."
    make -j$(nproc) >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Compile failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "Core library compiled successfully"
    return 0
}

build_web() {
    log_info "=========================================="
    log_info "Building web interface (face_db_web)"
    log_info "=========================================="

    if [ ! -d "$WEB_DIR" ]; then
        log_error "Web directory not found: $WEB_DIR"
        return 1
    fi

    local build_dir="$WEB_DIR/build"
    mkdir -p "$build_dir"

    cd "$build_dir"

    cmake "$WEB_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PROJECT_ROOT/install" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configuration failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Compiling..."
    make -j$(nproc) >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Compile failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Installing to $PROJECT_ROOT/install ..."
    make install >> "$BUILD_LOG" 2>&1
    if [ $? -ne 0 ]; then
        log_error "Install failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "Web interface compiled and installed"
    return 0
}

build_standalone() {
    log_info "=========================================="
    log_info "Building standalone C++ app (face_recognition_standalone)"
    log_info "=========================================="

    if [ ! -d "$STANDALONE_DIR" ]; then
        log_error "Standalone directory not found: $STANDALONE_DIR"
        return 1
    fi

    local build_dir="$STANDALONE_DIR/build"
    mkdir -p "$build_dir"

    cd "$build_dir"

    cmake "$STANDALONE_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$PROJECT_ROOT/install" \
        -DFACE_RECOGNITION_VENDORED_PREFIX="$FACE_RECOGNITION_VENDORED_PREFIX" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configuration failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Compiling..."
    make -j$(nproc) >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Compile failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "Standalone app compiled successfully"
    log_info "Binary : $build_dir/face_recognition_app"
    return 0
}

build_ros1() {
    log_info "=========================================="
    log_info "Building ROS1 version (face_recognition_ros1)"
    log_info "=========================================="

    if ! check_ros_environment "ROS1"; then
        log_error "ROS1 environment not available"
        return 1
    fi

    if [ ! -d "$ROS1_DIR" ]; then
        log_error "ROS1 directory not found: $ROS1_DIR"
        return 1
    fi

    source /opt/ros/noetic/setup.bash

    cd "$ROS1_DIR"

    catkin_make \
        -DCMAKE_BUILD_TYPE=Release \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "ROS1 build failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "ROS1 version compiled successfully"
    log_info "Source: source $ROS1_DIR/devel/setup.bash"
    return 0
}

build_ros2() {
    log_info "=========================================="
    log_info "Building ROS2 version (face_recognition_ros2)"
    log_info "=========================================="

    if ! check_ros_environment "ROS2"; then
        log_error "ROS2 environment not available"
        return 1
    fi

    if [ ! -d "$ROS2_DIR" ]; then
        log_error "ROS2 directory not found: $ROS2_DIR"
        return 1
    fi

    source /opt/ros/humble/setup.bash

    cd "$PROJECT_ROOT"

    colcon build \
        --packages-select face_recognition_core face_recognition_ros2_interfaces face_recognition_ros2 \
        --cmake-args -DCMAKE_BUILD_TYPE=Release \
        --event-handlers console_direct+ \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "ROS2 build failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "ROS2 version compiled successfully"
    log_info "Source: source $PROJECT_ROOT/install/setup.bash"
    return 0
}

verify_build() {
    local target="$1"
    local success=true

    case "$target" in
        CORE)
            if [ -f "$CORE_DIR/build/libface_recognition_core.so" ]; then
                log_success "Core library verified"
            else
                log_error "Core library verification failed"
                success=false
            fi
            ;;
        WEB)
            if [ -f "$WEB_DIR/build/face_db_web" ]; then
                log_success "Web server verified"
            else
                log_error "Web server verification failed"
                success=false
            fi
            ;;
        STANDALONE)
            if [ -f "$STANDALONE_DIR/build/face_recognition_app" ]; then
                log_success "Standalone app verified"
            else
                log_error "Standalone app verification failed"
                success=false
            fi
            ;;
        MODELS)
            if [ -f "$PROJECT_ROOT/models/det_10g.onnx" ] && [ -f "$PROJECT_ROOT/models/w600k_r50.onnx" ]; then
                log_success "Models verified"
            else
                log_error "Models verification failed"
                success=false
            fi
            ;;
        ROS1)
            if [ -f "$ROS1_DIR/devel/lib/face_recognition_ros1/face_recognition_node" ]; then
                log_success "ROS1 node verified"
            else
                log_error "ROS1 node verification failed"
                success=false
            fi
            ;;
        ROS2)
            if [ -f "$PROJECT_ROOT/install/face_recognition_ros2/lib/face_recognition_ros2/face_recognition_node" ]; then
                log_success "ROS2 node verified"
            else
                log_error "ROS2 node verification failed"
                success=false
            fi
            ;;
        ALL)
            verify_build "MODELS"
            verify_build "CORE"
            verify_build "WEB"
            verify_build "STANDALONE"
            if check_ros_environment "ROS1" 2>/dev/null; then verify_build "ROS1"; fi
            if check_ros_environment "ROS2" 2>/dev/null; then verify_build "ROS2"; fi
            ;;
    esac

    if [ "$success" = "true" ]; then
        return 0
    else
        return 1
    fi
}

main() {
    echo "========================================" | tee -a "$BUILD_LOG"
    echo "Face Recognition Node Build Script" | tee -a "$BUILD_LOG"
    echo "Start time: $(date)" | tee -a "$BUILD_LOG"
    echo "========================================" | tee -a "$BUILD_LOG"

    > "$BUILD_LOG"

    local build_target="${1:-ALL}"
    local build_success=true

    case "$build_target" in
        HELP|--help|-h)
            print_usage
            exit 0
            ;;
        CLEAN)
            clean_build
            exit 0
            ;;
    esac

    log_info "Build target: $build_target"

    check_dependencies "$build_target" || exit 1

    case "$build_target" in
        CORE)
            build_vendored_sqlite && build_success=true || build_success=false
            setup_vendored_rpath
            if [ "$build_success" = "true" ]; then
                build_core && build_success=true || build_success=false
            fi
            ;;
        WEB)
            build_vendored_sqlite && build_success=true || build_success=false
            setup_vendored_rpath
            if [ "$build_success" = "true" ]; then
                build_web && build_success=true || build_success=false
            fi
            ;;
        STANDALONE)
            build_vendored_sqlite && build_success=true || build_success=false
            setup_vendored_rpath
            if [ "$build_success" = "true" ]; then
                build_core && build_success=true || build_success=false
            fi
            if [ "$build_success" = "true" ]; then
                build_standalone && build_success=true || build_success=false
            fi
            ;;
        MODELS)
            download_models && build_success=true || build_success=false
            ;;
        ROS1)
            build_vendored_sqlite && build_success=true || build_success=false
            setup_vendored_rpath
            if [ "$build_success" = "true" ]; then
                build_core && build_success=true || build_success=false
            fi
            if [ "$build_success" = "true" ]; then
                build_ros1 && build_success=true || build_success=false
            fi
            ;;
        ROS2)
            build_vendored_sqlite && build_success=true || build_success=false
            setup_vendored_rpath
            if [ "$build_success" = "true" ]; then
                build_core && build_success=true || build_success=false
            fi
            if [ "$build_success" = "true" ]; then
                build_ros2 && build_success=true || build_success=false
            fi
            ;;
        ALL)
            log_info "Building all modules..."

            download_models && build_success=true || build_success=true   # non-fatal

            build_vendored_sqlite && build_success=true || build_success=false
            setup_vendored_rpath

            if [ "${BUILD_SPATIALITE:-OFF}" = "ON" ]; then
                if [ "$build_success" = "true" ]; then
                    build_vendored_spatialite && build_success=true || build_success=false
                fi
            fi

            if [ "$build_success" = "true" ]; then
                build_core && build_success=true || build_success=false
            fi

            if [ "$build_success" = "true" ]; then
                build_web && build_success=true || build_success=false
            fi

            if [ "$build_success" = "true" ]; then
                build_standalone && build_success=true || build_success=false
            fi

            if [ "$build_success" = "true" ]; then
                build_ros1 || { log_error "ROS1 build failed"; build_success=false; }
            fi

            if [ "$build_success" = "true" ]; then
                build_ros2 || { log_error "ROS2 build failed"; build_success=false; }
            fi
            ;;
        *)
            log_error "Unknown build target: $build_target"
            print_usage
            exit 1
            ;;
    esac

    echo "========================================" | tee -a "$BUILD_LOG"
    echo "Build end time: $(date)" | tee -a "$BUILD_LOG"
    echo "========================================" | tee -a "$BUILD_LOG"

    if [ "$build_success" = "true" ]; then
        log_success "Build complete! Log: $BUILD_LOG"
        echo -e "${GREEN}Build successful${NC}"
        exit 0
    else
        log_error "Build had errors, see log: $BUILD_LOG"
        echo -e "${RED}Build failed${NC}"
        exit 1
    fi
}

main "$@"
