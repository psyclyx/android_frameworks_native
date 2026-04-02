#!/bin/bash
# Integration test for the SurfaceFlinger Wayland compositor.
#
# Usage:
#   ./run_integration_test.sh [--build-only] [--skip-build]
#
# Prerequisites:
#   - ANDROID_BUILD_TOP set and lunch target configured
#   - For runtime tests: Cuttlefish instance running, adb connected
#
# Exit codes:
#   0 = all tests passed
#   1 = build failure
#   2 = runtime failure

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SF_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_ONLY=false
SKIP_BUILD=false
MAX_FRAMES=60

for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=true ;;
        --skip-build) SKIP_BUILD=true ;;
        --frames=*) MAX_FRAMES="${arg#--frames=}" ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

pass() { echo -e "${GREEN}PASS${NC}: $1"; }
fail() { echo -e "${RED}FAIL${NC}: $1"; exit "$2"; }
info() { echo -e "${YELLOW}INFO${NC}: $1"; }

# ---------- Build verification ----------

build_test() {
    info "Building surfaceflinger (with Wayland compositor)..."
    if ! (cd "$SF_DIR" && mma -j"$(nproc)" 2>&1 | tail -20); then
        fail "surfaceflinger build failed" 1
    fi
    pass "surfaceflinger build"

    info "Building wayland_egl_test and wayland_shm_test..."
    if ! (cd "$SCRIPT_DIR" && mma -j"$(nproc)" 2>&1 | tail -20); then
        fail "test client build failed" 1
    fi
    pass "wayland_egl_test build"
    pass "wayland_shm_test build"
}

# ---------- Static checks ----------

static_checks() {
    info "Checking Wayland module files exist..."
    local modules=(
        WaylandCompositor
        WaylandDmabuf
        WaylandSurface
        WaylandXdgShell
        WaylandOutput
        WaylandSeat
        WaylandShm
    )
    for mod in "${modules[@]}"; do
        if [[ ! -f "$SF_DIR/Wayland/${mod}.h" ]] || [[ ! -f "$SF_DIR/Wayland/${mod}.cpp" ]]; then
            fail "Missing Wayland/${mod}.{h,cpp}" 1
        fi
    done
    pass "All Wayland module files present"

    info "Checking Android.bp references Wayland sources..."
    if ! grep -q "Wayland/WaylandCompositor.cpp" "$SF_DIR/Android.bp"; then
        fail "Android.bp does not reference WaylandCompositor.cpp" 1
    fi
    pass "Android.bp references Wayland sources"

    info "Checking test client Android.bp..."
    if ! grep -q "wayland_egl_test" "$SCRIPT_DIR/Android.bp"; then
        fail "Test client Android.bp missing wayland_egl_test target" 1
    fi
    if ! grep -q "wayland_shm_test" "$SCRIPT_DIR/Android.bp"; then
        fail "Test client Android.bp missing wayland_shm_test target" 1
    fi
    pass "Test client build targets exist"
}

# ---------- Runtime test ----------

runtime_test() {
    info "Checking adb connectivity..."
    if ! adb devices 2>/dev/null | grep -q "device$"; then
        fail "No adb device connected. Start a Cuttlefish instance first." 2
    fi
    pass "adb device connected"

    info "Checking Wayland socket..."
    local socket_path="/data/wayland/wayland-0"
    if ! adb shell "test -S $socket_path" 2>/dev/null; then
        info "Wayland socket not found at $socket_path"
        info "Attempting to verify SurfaceFlinger has Wayland support..."
        if adb shell "ls /data/wayland/" 2>/dev/null; then
            fail "Socket directory exists but socket missing. SF may have failed to create it." 2
        else
            info "Creating /data/wayland directory..."
            adb shell "mkdir -p /data/wayland && chmod 0700 /data/wayland"
            info "Wayland directory created. SF must be restarted to create socket."
            info "Run: adb shell stop && adb shell start"
            fail "Wayland socket not yet created. Restart SF and re-run." 2
        fi
    fi
    pass "Wayland socket exists"

    info "Pushing wayland_egl_test to device..."
    local test_bin
    test_bin="$(find "$ANDROID_PRODUCT_OUT" -name wayland_egl_test -type f 2>/dev/null | head -1)"
    if [[ -z "$test_bin" ]]; then
        fail "wayland_egl_test binary not found in \$ANDROID_PRODUCT_OUT. Build first." 2
    fi
    adb push "$test_bin" /data/local/tmp/wayland_egl_test
    adb shell "chmod 755 /data/local/tmp/wayland_egl_test"
    pass "Test binary pushed"

    info "Running wayland_egl_test ($MAX_FRAMES frames)..."
    local output
    local exit_code=0
    output=$(adb shell "XDG_RUNTIME_DIR=/data/wayland WAYLAND_DISPLAY=wayland-0 \
        /data/local/tmp/wayland_egl_test $MAX_FRAMES" 2>&1) || exit_code=$?

    echo "$output"

    if [[ $exit_code -ne 0 ]]; then
        fail "wayland_egl_test exited with code $exit_code" 2
    fi

    # Validate output
    if ! echo "$output" | grep -q "Connected to Wayland display"; then
        fail "Test did not connect to Wayland display" 2
    fi
    pass "Connected to Wayland display"

    if ! echo "$output" | grep -q "EGL.*initialized"; then
        fail "EGL initialization failed" 2
    fi
    pass "EGL initialized"

    if ! echo "$output" | grep -q "GL_RENDERER"; then
        fail "GL context not created" 2
    fi
    pass "GL context created"

    if ! echo "$output" | grep -q "Done\. Rendered"; then
        fail "Render loop did not complete" 2
    fi

    local rendered
    rendered=$(echo "$output" | grep -oP 'Rendered \K[0-9]+')
    if [[ -n "$rendered" ]] && [[ "$rendered" -ge "$MAX_FRAMES" ]]; then
        pass "Rendered $rendered frames"
    else
        fail "Expected $MAX_FRAMES frames, got ${rendered:-0}" 2
    fi

    # Optional: screenshot check
    info "Taking screenshot for visual verification..."
    local screenshot="/tmp/wayland_test_screenshot.png"
    if adb shell screencap -p > "$screenshot" 2>/dev/null; then
        local size
        size=$(wc -c < "$screenshot")
        if [[ "$size" -gt 1000 ]]; then
            pass "Screenshot captured ($size bytes) — manual visual check: $screenshot"
        else
            info "Screenshot too small ($size bytes), may be empty"
        fi
    else
        info "screencap not available, skipping visual check"
    fi

    # --- SHM test ---
    info "Pushing wayland_shm_test to device..."
    local shm_bin
    shm_bin="$(find "$ANDROID_PRODUCT_OUT" -name wayland_shm_test -type f 2>/dev/null | head -1)"
    if [[ -z "$shm_bin" ]]; then
        fail "wayland_shm_test binary not found in \$ANDROID_PRODUCT_OUT. Build first." 2
    fi
    adb push "$shm_bin" /data/local/tmp/wayland_shm_test
    adb shell "chmod 755 /data/local/tmp/wayland_shm_test"
    pass "SHM test binary pushed"

    info "Running wayland_shm_test ($MAX_FRAMES frames)..."
    local shm_output
    local shm_exit=0
    shm_output=$(adb shell "XDG_RUNTIME_DIR=/data/wayland WAYLAND_DISPLAY=wayland-0 \
        /data/local/tmp/wayland_shm_test $MAX_FRAMES" 2>&1) || shm_exit=$?

    echo "$shm_output"

    if [[ $shm_exit -ne 0 ]]; then
        fail "wayland_shm_test exited with code $shm_exit" 2
    fi

    if ! echo "$shm_output" | grep -q "Connected to Wayland display"; then
        fail "SHM test did not connect to Wayland display" 2
    fi
    pass "SHM: Connected to Wayland display"

    if ! echo "$shm_output" | grep -q "SHM pool created"; then
        fail "SHM pool creation failed" 2
    fi
    pass "SHM: Pool created"

    if ! echo "$shm_output" | grep -q "Done\. Rendered"; then
        fail "SHM render loop did not complete" 2
    fi

    local shm_rendered
    shm_rendered=$(echo "$shm_output" | grep -oP 'Rendered \K[0-9]+')
    if [[ -n "$shm_rendered" ]] && [[ "$shm_rendered" -ge "$MAX_FRAMES" ]]; then
        pass "SHM: Rendered $shm_rendered frames"
    else
        fail "SHM: Expected $MAX_FRAMES frames, got ${shm_rendered:-0}" 2
    fi
}

# ---------- Main ----------

echo "===== Wayland Compositor Integration Test ====="
echo ""

static_checks

if [[ "$SKIP_BUILD" != true ]]; then
    if [[ -z "${ANDROID_BUILD_TOP:-}" ]]; then
        fail "ANDROID_BUILD_TOP not set. Run 'source build/envsetup.sh && lunch <target>' first." 1
    fi
    build_test
else
    info "Skipping build (--skip-build)"
fi

if [[ "$BUILD_ONLY" == true ]]; then
    info "Skipping runtime tests (--build-only)"
    echo ""
    echo -e "${GREEN}Build verification complete.${NC}"
    exit 0
fi

runtime_test

echo ""
echo -e "${GREEN}===== All integration tests passed =====${NC}"
exit 0
