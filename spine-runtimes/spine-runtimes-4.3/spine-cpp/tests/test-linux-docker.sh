#!/bin/bash
# Spine-C++ Docker Test Runner
#
# Runs the full spine-cpp test suite inside a Linux Docker container.
# This ensures consistent testing across different host environments.

set -e

# Change to spine-runtimes root directory (parent of spine-cpp)
cd "$(dirname "$0")/../.."

# Source logging utilities
source formatters/logging/logging.sh

log_title "Spine-C++ Docker Test"
log_detail "Running tests in Linux container"

# Detect existing build type
BUILD_TYPE=""
cd spine-cpp
if [ -f "build/CMakeCache.txt" ]; then
    if grep -q "CMAKE_BUILD_TYPE:STRING=Release" build/CMakeCache.txt 2>/dev/null; then
        BUILD_TYPE="release"
    elif grep -q "CMAKE_BUILD_TYPE:STRING=Debug" build/CMakeCache.txt 2>/dev/null; then
        BUILD_TYPE="debug"
    fi
    log_detail "Detected existing build type: ${BUILD_TYPE:-unknown}"
fi
cd ..

# Use a lightweight Linux image with build tools
DOCKER_IMAGE="gcc:12"

log_action "Pulling Docker image"
if docker pull "$DOCKER_IMAGE" > /dev/null 2>&1; then
    log_ok
else
    log_fail "Failed to pull Docker image"
    exit 1
fi

log_action "Running tests in container"
if DOCKER_OUTPUT=$(docker run --rm \
    -v "$(pwd)":/workspace \
    -w /workspace/spine-cpp \
    "$DOCKER_IMAGE" \
    bash -c "apt-get update -qq && apt-get install -y -qq cmake ninja-build && ./tests/test.sh" 2>&1); then
    log_ok
    
    log_action "Analyzing executables"
    if ANALYSIS_OUTPUT=$(docker run --rm \
        -v "$(pwd)":/workspace \
        -w /workspace/spine-cpp \
        "$DOCKER_IMAGE" \
        bash -c "echo '=== EXECUTABLE ANALYSIS ==='; for exe in build/headless-test*; do if [ -f \"\$exe\" ] && [ -x \"\$exe\" ]; then echo; echo \"--- \$(basename \"\$exe\") ---\"; echo \"Size: \$(du -h \"\$exe\" | cut -f1)\"; echo \"Dependencies:\"; ldd \"\$exe\" 2>/dev/null || echo 'No dynamic dependencies'; fi; done" 2>&1); then
        log_ok
        log_detail "$ANALYSIS_OUTPUT"
    else
        log_fail
        log_error_output "$ANALYSIS_OUTPUT"
    fi
    
    log_action "Restoring local build directory"
    cd spine-cpp
    if rm -rf build; then
        if [ -n "$BUILD_TYPE" ]; then
            log_detail "Rebuilding $BUILD_TYPE configuration"
            if ./build.sh clean "$BUILD_TYPE" > /dev/null 2>&1; then
                log_ok "Local build directory restored ($BUILD_TYPE)"
            else
                log_detail "Warning: Could not restore $BUILD_TYPE build"
            fi
        else
            log_detail "No previous build detected, leaving clean"
            log_ok "Build directory cleaned"
        fi
    else
        log_detail "Warning: Could not remove build directory"
    fi
else
    log_fail
    log_error_output "$DOCKER_OUTPUT"
    exit 1
fi