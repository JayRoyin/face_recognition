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

# -----------------------------------------------------------------------------
# Build layout — every artifact stays under the project root; nothing is written
# inside a source package directory.
#
#     build/<target>/            intermediate build trees (gitignored)
#     install/bin|lib|include/   the single place users pick binaries from
#     install/ros1|ros2/         ROS workspaces (colcon / catkin need their own
#                                install space, so they get a dedicated subtree
#                                instead of being mixed into install/)
#     log/                       logs
#
# Rationale: build trees used to live next to each package
# (src/face_recognition_core/build, src/face_db_web/build, ...), which made the
# same core library get compiled once per consumer and left binaries scattered
# across the tree. Keeping one build root and one install root makes
# "what did I actually build?" answerable with a single `ls`.
# -----------------------------------------------------------------------------
BUILD_ROOT="$PROJECT_ROOT/build"
INSTALL_ROOT="$PROJECT_ROOT/install"
LOG_ROOT="$PROJECT_ROOT/log"

BUILD_CORE_DIR="$BUILD_ROOT/core"
BUILD_WEB_DIR="$BUILD_ROOT/web"
BUILD_STANDALONE_DIR="$BUILD_ROOT/standalone"
BUILD_VENDORED_DIR="$BUILD_ROOT/vendored"
BUILD_TP_SQLITE_DIR="$BUILD_VENDORED_DIR/sqlite3"
BUILD_TP_SPATIALITE_DIR="$BUILD_VENDORED_DIR/spatialite"
BUILD_ROS1_DIR="$BUILD_ROOT/ros1"
BUILD_ROS2_DIR="$BUILD_ROOT/ros2"

INSTALL_ROS2_DIR="$INSTALL_ROOT/ros2"

ROS1_INTERFACES_DIR="$SRC_DIR/face_recognition_ros1_interfaces"

# Vendored SQLite prefix — exported so consumer CMakeLists can pick it up.
export FACE_RECOGNITION_VENDORED_PREFIX="$INSTALL_ROOT/vendored"

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

    # Do NOT invent a proxy here. This used to default all_proxy to
    # http://127.0.0.1:7890, so on any machine that does not happen to run a
    # proxy on that port every download timed out, and the only visible symptom
    # was "download failed" — a developer-local preference baked into the build.
    if [ -n "${all_proxy:-}${ALL_PROXY:-}" ]; then
        log_info "Using proxy from environment: ${all_proxy:-$ALL_PROXY}"
    else
        log_info "No proxy set (use all_proxy=http://host:port if this host needs one)"
    fi

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
        # current layout
        "$BUILD_ROOT"
        "$INSTALL_ROOT"
        "$LOG_ROOT"
        "$PROJECT_ROOT/devel"
        # legacy layout: build trees that used to live next to each package
        "$CORE_DIR/build"   "$CORE_DIR/install"   "$CORE_DIR/log"
        "$ROS1_DIR/build"   "$ROS1_DIR/devel"     "$ROS1_DIR/install" "$ROS1_DIR/log"
        "$ROS2_DIR/build"   "$ROS2_DIR/install"   "$ROS2_DIR/log"
        "$WEB_DIR/build"
        "$STANDALONE_DIR/build"
        "$TP_SQLITE_DIR/build"
        "$TP_SPATIALITE_DIR/build"
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
    local build_dir="$BUILD_TP_SQLITE_DIR"
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
    local build_dir="$BUILD_TP_SPATIALITE_DIR"
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

    local build_dir="$BUILD_CORE_DIR"
    mkdir -p "$build_dir"

    # Out-of-source configure + build into the shared build root, then install
    # into the single install root so every consumer can link the SAME
    # libface_recognition_core.so instead of compiling its own copy.
    cmake -S "$CORE_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_ROOT" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configuration failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Compiling..."
    cmake --build "$build_dir" -j"$(nproc)" >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Compile failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Installing to $INSTALL_ROOT ..."
    cmake --install "$build_dir" >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Install failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "Core library compiled and installed"
    log_info "Library: $INSTALL_ROOT/lib/libface_recognition_core.so*"
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

    local build_dir="$BUILD_WEB_DIR"
    mkdir -p "$build_dir"

    cmake -S "$WEB_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_ROOT" \
        -DCMAKE_PREFIX_PATH="$INSTALL_ROOT" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configuration failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Compiling..."
    cmake --build "$build_dir" -j"$(nproc)" >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Compile failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Installing to $INSTALL_ROOT ..."
    cmake --install "$build_dir" >> "$BUILD_LOG" 2>&1
    if [ $? -ne 0 ]; then
        log_error "Install failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "Web interface compiled and installed"
    log_info "Binary : $INSTALL_ROOT/bin/face_db_web"
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

    local build_dir="$BUILD_STANDALONE_DIR"
    mkdir -p "$build_dir"

    cmake -S "$STANDALONE_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$INSTALL_ROOT" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_ROOT" \
        -DFACE_RECOGNITION_VENDORED_PREFIX="$FACE_RECOGNITION_VENDORED_PREFIX" \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "CMake configuration failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Compiling..."
    cmake --build "$build_dir" -j"$(nproc)" >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "Compile failed, see log: $BUILD_LOG"
        return 1
    fi

    log_info "Installing to $INSTALL_ROOT ..."
    cmake --install "$build_dir" >> "$BUILD_LOG" 2>&1
    if [ $? -ne 0 ]; then
        log_error "Install failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "Standalone app compiled and installed"
    log_info "Binary : $INSTALL_ROOT/bin/face_recognition_app"
    return 0
}

run_tests() {
    log_info "=========================================="
    log_info "Running regression tests (non-ROS front-end)"
    log_info "=========================================="

    local script="$PROJECT_ROOT/tests/regression.sh"
    if [ ! -f "$script" ]; then
        log_error "Test script not found: $script"
        return 1
    fi
    if [ ! -x "$INSTALL_ROOT/bin/face_recognition_app" ]; then
        log_error "face_recognition_app is missing — run ./build.sh STANDALONE first"
        return 1
    fi

    chmod +x "$script"

    # The suite drives the installed binary only, and each test creates its own
    # temp database — it never reads or writes the operator's gallery.
    if ! FACE_APP="$INSTALL_ROOT/bin/face_recognition_app" "$script"; then
        log_error "Regression tests failed"
        return 1
    fi

    log_success "Regression tests passed"
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

    # Make the installed face_recognition_core package discoverable. Exported
    # rather than passed as -DCMAKE_PREFIX_PATH: catkin_make builds its own
    # prefix path from the environment plus the workspace, and a hard -D would
    # replace it and hide the sibling catkin packages.
    export CMAKE_PREFIX_PATH="$INSTALL_ROOT:${CMAKE_PREFIX_PATH:-}"

    # Keep a conda prefix out of the dependency resolution — see the long note in
    # build_ros2() for the libgdal/libcurl failure this prevents.
    _conda_prefix="${CONDA_PREFIX:-}"
    unset CONDA_PREFIX
    unset PYTHON_EXECUTABLE PYTHON_LIBRARY PYTHON_INCLUDE_DIR
    _ros_ignore_args=""
    if [ -n "$_conda_prefix" ] && [ -d "$_conda_prefix" ]; then
        _ros_ignore_args="-DCMAKE_IGNORE_PREFIX_PATH=$_conda_prefix"
    fi
    _ros_python_args=""
    if [ -x /usr/bin/python3 ]; then
        _ros_python_args="-DPython3_EXECUTABLE=/usr/bin/python3 -DPython3_ROOT_DIR=/usr \
-DPYTHON_EXECUTABLE=/usr/bin/python3 -DPYTHON_INCLUDE_DIR=/usr/include/python3.10 \
-DPYTHON_LIBRARY=/usr/lib/x86_64-linux-gnu/libpython3.10.so"
    fi

    # `catkin_make` expects a workspace ROOT containing src/<packages>. Running
    # it inside a package directory — which is what this script used to do —
    # makes it look for packages in the package's own src/ and find none.
    # Build a real workspace under the shared build root instead, with the ROS1
    # packages symlinked into its src/ so the source tree stays untouched and
    # build/ + devel/ end up under build/ros1/.
    #
    # NOTE: ROS1 is not installed on the current development host, so this path
    # is not compile-verified here.
    mkdir -p "$BUILD_ROS1_DIR/src"
    ln -sfn "$ROS1_DIR"            "$BUILD_ROS1_DIR/src/face_recognition_ros1"
    ln -sfn "$ROS1_INTERFACES_DIR" "$BUILD_ROS1_DIR/src/face_recognition_ros1_interfaces"

    cd "$BUILD_ROS1_DIR"

    catkin_make \
        -DCMAKE_BUILD_TYPE=Release \
        $_ros_ignore_args $_ros_python_args \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "ROS1 build failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "ROS1 version compiled successfully"
    log_info "Workspace: $BUILD_ROS1_DIR"
    log_info "Source: source $BUILD_ROS1_DIR/devel/setup.bash"
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

    # Make the installed face_recognition_core package discoverable. Exported
    # rather than passed as -DCMAKE_PREFIX_PATH: ament already assembles its own
    # prefix path (workspace + dependencies) and a hard -D would replace it and
    # break finding face_recognition_ros2_interfaces.
    export CMAKE_PREFIX_PATH="$INSTALL_ROOT:${CMAKE_PREFIX_PATH:-}"

    # Force the SYSTEM Python. If a conda prefix is active, CMake finds conda's
    # python3, links $CONDA_PREFIX/lib/libpython3.*.so, and then emits
    #   -Wl,-rpath-link,$CONDA_PREFIX/lib
    # ld searches that directory FIRST when resolving the DT_NEEDED libraries of
    # the system libgdal.so that OpenCV pulls in, finds conda's libcurl (which
    # does not provide CURL_OPENSSL_4) and the link dies with
    #   /lib/libgdal.so.30: undefined reference to `curl_easy_cleanup@CURL_OPENSSL_4'
    # The host libraries are fine — the wrong directory is being searched. ROS2
    # Humble also targets Python 3.10, so pointing at conda's 3.13 is wrong
    # regardless.
    # A conda prefix must not contribute ANY dependency to this build. It ships
    # its own libpython, libcurl, libtiff, spdlog and fmt, and CMake then records
    # $CONDA_PREFIX/lib in the link line's -rpath AND -rpath-link. `ld` searches
    # that directory FIRST when resolving the DT_NEEDED libraries of the system
    # libgdal.so that OpenCV pulls in, finds conda's libcurl (which provides no
    # CURL_OPENSSL_4 symbol version) and the link fails with
    #   /lib/libgdal.so.30: undefined reference to `curl_easy_cleanup@CURL_OPENSSL_4'
    # Nothing is wrong with the host libraries — the wrong directory is searched.
    # (CMAKE_IGNORE_PREFIX_PATH requires CMake >= 3.23.)
    _conda_prefix="${CONDA_PREFIX:-}"
    unset CONDA_PREFIX
    unset PYTHON_EXECUTABLE PYTHON_LIBRARY PYTHON_INCLUDE_DIR
    _ros_ignore_args=""
    if [ -n "$_conda_prefix" ] && [ -d "$_conda_prefix" ]; then
        _ros_ignore_args="-DCMAKE_IGNORE_PREFIX_PATH=$_conda_prefix"
    fi
    _ros_python_args=""
    if [ -x /usr/bin/python3 ]; then
        _ros_python_args="-DPython3_EXECUTABLE=/usr/bin/python3 -DPython3_ROOT_DIR=/usr"
    fi
    # rosidl_generator_py resolves Python through python_cmake_module, i.e. the
    # LEGACY FindPythonInterp / FindPythonLibs variables (PYTHON_EXECUTABLE,
    # PYTHON_LIBRARY, PYTHON_INCLUDE_DIR) — NOT Python3_*. Pinning only Python3_*
    # leaves conda free to win, which links $CONDA_PREFIX/lib/libpython3.13.so
    # and makes CMake emit -Wl,-rpath-link,$CONDA_PREFIX/lib. That directory is
    # then searched FIRST for the DT_NEEDED libraries of the system libgdal.so
    # that OpenCV pulls in, and conda's libcurl (no CURL_OPENSSL_4) shadows the
    # system one -> "undefined reference to `curl_easy_cleanup@CURL_OPENSSL_4'".
    for _p in /usr/lib/x86_64-linux-gnu /usr/lib; do
        if [ -e "$_p/libpython3.10.so" ]; then
            _ros_python_args="$_ros_python_args -DPYTHON_LIBRARY=$_p/libpython3.10.so"
            break
        fi
    done
    if [ -f /usr/include/python3.10/Python.h ]; then
        _ros_python_args="$_ros_python_args -DPYTHON_INCLUDE_DIR=/usr/include/python3.10"
    fi
    if [ -x /usr/bin/python3 ]; then
        _ros_python_args="$_ros_python_args -DPYTHON_EXECUTABLE=/usr/bin/python3"
    fi

    # Build bases are pinned under the project build root so that colcon no
    # longer writes build/ install/ log/ next to the sources and no longer
    # shares install/ with the plain-CMake targets (which would put two
    # different layouts in one directory).
    #
    # `face_recognition_core` is deliberately NOT selected: face_recognition_ros2
    # already pulls the core sources in through add_subdirectory(), so selecting
    # it as a package as well compiled the same library twice per build and left
    # an unused copy in the install space.
    cd "$PROJECT_ROOT"

    # NOTE: --log-base is a GLOBAL colcon option and must precede the
    # subcommand; --build-base/--install-base/--base-paths belong to `build`.
    #
    # `face_recognition_core` is NOT selected: face_recognition_ros2 links the
    # installed core package instead of compiling its own copy.
    colcon --log-base "$LOG_ROOT/ros2" build \
        --base-paths "$SRC_DIR" \
        --packages-select face_recognition_ros2_interfaces face_recognition_ros2 \
        --build-base "$BUILD_ROS2_DIR" \
        --install-base "$INSTALL_ROS2_DIR" \
        --cmake-args -DCMAKE_BUILD_TYPE=Release $_ros_python_args $_ros_ignore_args \
        --event-handlers console_direct+ \
        >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        log_error "ROS2 build failed, see log: $BUILD_LOG"
        return 1
    fi

    log_success "ROS2 version compiled successfully"
    log_info "Install space: $INSTALL_ROS2_DIR"
    log_info "Source: source $INSTALL_ROS2_DIR/setup.bash"
    return 0
}

verify_build() {
    local target="$1"
    local success=true

    case "$target" in
        CORE)
            if compgen -G "$INSTALL_ROOT/lib/libface_recognition_core.so*" > /dev/null; then
                log_success "Core library verified"
            else
                log_error "Core library verification failed (expected $INSTALL_ROOT/lib/libface_recognition_core.so*)"
                success=false
            fi
            ;;
        WEB)
            if [ -f "$INSTALL_ROOT/bin/face_db_web" ]; then
                log_success "Web server verified"
            else
                log_error "Web server verification failed (expected $INSTALL_ROOT/bin/face_db_web)"
                success=false
            fi
            ;;
        STANDALONE)
            if [ -f "$INSTALL_ROOT/bin/face_recognition_app" ]; then
                log_success "Standalone app verified"
            else
                log_error "Standalone app verification failed (expected $INSTALL_ROOT/bin/face_recognition_app)"
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
            if [ -f "$BUILD_ROS1_DIR/devel/lib/face_recognition_ros1/face_recognition_node" ]; then
                log_success "ROS1 node verified"
            else
                log_error "ROS1 node verification failed (expected $BUILD_ROS1_DIR/devel/lib/face_recognition_ros1/face_recognition_node)"
                success=false
            fi
            ;;
        ROS2)
            if [ -f "$INSTALL_ROS2_DIR/face_recognition_ros2/lib/face_recognition_ros2/face_recognition_node" ]; then
                log_success "ROS2 node verified"
            else
                log_error "ROS2 node verification failed (expected $INSTALL_ROS2_DIR/face_recognition_ros2/lib/face_recognition_ros2/face_recognition_node)"
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

print_artifacts() {
    echo ""
    log_info "Build layout:"
    log_info "  build trees : $BUILD_ROOT/{core,web,standalone,vendored,ros1,ros2}"
    log_info "  install root: $INSTALL_ROOT"
    local core_lib=false
    if compgen -G "$INSTALL_ROOT/lib/libface_recognition_core.so*" > /dev/null; then
        log_info "  artifact    : $INSTALL_ROOT/lib/libface_recognition_core.so*"
        core_lib=true
    fi
    local candidates=(
        "$INSTALL_ROOT/bin/face_recognition_app"
        "$INSTALL_ROOT/bin/face_db_web"
        "$INSTALL_ROS2_DIR/face_recognition_ros2/lib/face_recognition_ros2/face_recognition_node"
        "$BUILD_ROS1_DIR/devel/lib/face_recognition_ros1/face_recognition_node"
    )
    local any=false
    for f in "${candidates[@]}"; do
        if [ -f "$f" ]; then
            log_info "  artifact    : $f"
            any=true
        fi
    done
    if [ "$any" = "false" ] && [ "$core_lib" = "false" ]; then
        log_warn "No artifacts found yet - did the build run?"
    fi
}

main() {
    # Truncate FIRST. The header used to be written with `tee -a` and then
    # immediately erased by the `> "$BUILD_LOG"` on the next line, so build.log
    # never contained the script name or the start time.
    : > "$BUILD_LOG"

    echo "========================================" | tee -a "$BUILD_LOG"
    echo "Face Recognition Node Build Script" | tee -a "$BUILD_LOG"
    echo "Start time: $(date)" | tee -a "$BUILD_LOG"
    echo "========================================" | tee -a "$BUILD_LOG"

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
        TEST)
            run_tests && build_success=true || build_success=false
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

            # ROS is optional: a host with only one of Noetic/Humble (or neither)
            # must still get a successful ALL build for the non-ROS targets.
            if [ "$build_success" = "true" ]; then
                if check_ros_environment "ROS1" 2>/dev/null; then
                    build_ros1 || { log_error "ROS1 build failed"; build_success=false; }
                else
                    log_warn "ROS1 (Noetic) not found - skipping ROS1 target"
                fi
            fi

            if [ "$build_success" = "true" ]; then
                if check_ros_environment "ROS2" 2>/dev/null; then
                    build_ros2 || { log_error "ROS2 build failed"; build_success=false; }
                else
                    log_warn "ROS2 (Humble) not found - skipping ROS2 target"
                fi
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

    print_artifacts

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
