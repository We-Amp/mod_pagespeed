#!/bin/bash
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# Run PageSpeed IIS integration tests on Windows via SSH or locally
# This script connects to the dockur/windows container and runs tests there,
# or runs tests locally if already on Windows (MSYS/Git Bash).
#
# Prerequisites for remote execution:
#   - Windows VM running (docker compose --profile windows-x64 up -d)
#   - SSH agent with keys (SSH_AUTH_SOCK set)
#   - Source code accessible on Windows (via SMB mount or copy)
#
# Prerequisites for local execution (on Windows):
#   - Running in MSYS/Git Bash or similar
#   - IIS installed (Windows Server feature)
#   - Python 3 with pytest installed

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Detect if running locally on Windows (MSYS/Git Bash)
IS_LOCAL_WINDOWS=false
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "mingw"* || "$OSTYPE" == "cygwin" ]]; then
    IS_LOCAL_WINDOWS=true
fi

# Configuration for Windows VM (dockur/windows)
WINDOWS_HOST="${WINDOWS_HOST:-localhost}"
WINDOWS_SSH_PORT="${WINDOWS_SSH_PORT:-2222}"
WINDOWS_USER="${WINDOWS_USER:-Developer}"
WINDOWS_PASSWORD="${WINDOWS_PASSWORD:-ChangeMe1}"
IIS_PORT="${IIS_PORT:-8080}"

# Test configuration
TEST_FILTER="${TEST_FILTER:-}"
BUILD_ONLY="${BUILD_ONLY:-false}"
SKIP_BUILD="${SKIP_BUILD:-false}"
KEEP_RUNNING="${KEEP_RUNNING:-false}"
VERBOSE="${VERBOSE:-false}"
NO_MODULE="${NO_MODULE:-false}"
USE_IIS_EXPRESS="${USE_IIS_EXPRESS:-false}"
LOCAL_MODE="${LOCAL_MODE:-$IS_LOCAL_WINDOWS}"

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] [TEST_FILTER]

Run PageSpeed IIS integration tests on Windows via SSH.

This script:
  1. Connects to the Windows VM via SSH
  2. Sets up Full IIS with the PageSpeed test site
  3. Optionally installs the PageSpeed native module
  4. Runs pytest tests against the IIS server

Options:
    -h, --help          Show this help message
    -b, --build-only    Only set up IIS, don't run tests
    -s, --skip-build    Skip IIS setup, just run tests
    -k, --keep-running  Keep IIS running after tests
    -v, --verbose       Show verbose test output
    --no-module         Don't install PageSpeed module (baseline test)
    --iis-express       Use IIS Express instead of Full IIS
    --host HOST         Windows VM hostname (default: localhost)
    --port PORT         Windows VM SSH port (default: 2222)
    --user USER         Windows VM username (default: Developer)
    --password PASS     Windows VM password (default: ChangeMe1)
    --iis-port PORT     Port for IIS to listen on (default: 8080)
    --local             Force local execution (auto-detected on Windows)
    --remote            Force remote execution via SSH

Examples:
    $0                          # Run all tests with Full IIS
    $0 sanity                   # Run tests matching "sanity"
    $0 --no-module              # Run without PageSpeed module (baseline)
    $0 --build-only             # Only set up IIS, no tests
    $0 --skip-build -k sanity   # Quick test iteration
    $0 --iis-express            # Use IIS Express (legacy)

Environment Variables:
    WINDOWS_HOST        Windows VM hostname
    WINDOWS_SSH_PORT    Windows VM SSH port
    WINDOWS_USER        Windows VM username
    WINDOWS_PASSWORD    Windows VM password
    IIS_PORT            IIS listening port
    SSH_AUTH_SOCK       SSH agent socket (required for key auth)
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -b|--build-only)
            BUILD_ONLY=true
            shift
            ;;
        -s|--skip-build)
            SKIP_BUILD=true
            shift
            ;;
        -k|--keep-running)
            KEEP_RUNNING=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        --no-module)
            NO_MODULE=true
            shift
            ;;
        --iis-express)
            USE_IIS_EXPRESS=true
            shift
            ;;
        --host)
            WINDOWS_HOST="$2"
            shift 2
            ;;
        --port)
            WINDOWS_SSH_PORT="$2"
            shift 2
            ;;
        --user)
            WINDOWS_USER="$2"
            shift 2
            ;;
        --password)
            WINDOWS_PASSWORD="$2"
            shift 2
            ;;
        --iis-port)
            IIS_PORT="$2"
            shift 2
            ;;
        --local)
            LOCAL_MODE=true
            shift
            ;;
        --remote)
            LOCAL_MODE=false
            shift
            ;;
        -*)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
        *)
            TEST_FILTER="$1"
            shift
            ;;
    esac
done

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

log_status() {
    echo -e "${CYAN}[IIS Tests]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[IIS Tests]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[IIS Tests]${NC} $1"
}

log_error() {
    echo -e "${RED}[IIS Tests]${NC} $1"
}

# ===== LOCAL EXECUTION FUNCTIONS (for running directly on Windows) =====

# Setup IIS locally (when running on Windows)
setup_iis_local() {
    if [[ "$USE_IIS_EXPRESS" == "true" ]]; then
        log_status "Setting up IIS Express..."
        local setup_script="setup_iis_test.ps1"
        local action="start"
    else
        log_status "Setting up Full IIS..."
        local setup_script="setup_iis_full.ps1"
        local action="install"
    fi

    local module_flag=""
    if [[ "$NO_MODULE" == "true" ]]; then
        module_flag="-NoModule"
    fi

    log_status "Running: $setup_script $action -Port $IIS_PORT $module_flag"
    powershell -ExecutionPolicy Bypass -File "$SCRIPT_DIR/$setup_script" $action -Port $IIS_PORT $module_flag

    # Verify server is responding
    log_status "Verifying IIS is responding..."
    local attempts=0
    local max_attempts=10
    while [[ $attempts -lt $max_attempts ]]; do
        if curl -s -o /dev/null -w "%{http_code}" "http://localhost:$IIS_PORT/" 2>/dev/null | grep -q "200"; then
            log_success "IIS is responding on port $IIS_PORT"
            return 0
        fi
        attempts=$((attempts + 1))
        sleep 2
    done

    log_error "IIS is not responding after $max_attempts attempts"
    return 1
}

# Stop IIS locally
stop_iis_local() {
    if [[ "$USE_IIS_EXPRESS" == "true" ]]; then
        log_status "Stopping IIS Express..."
        powershell -ExecutionPolicy Bypass -File "$SCRIPT_DIR/setup_iis_test.ps1" stop
    else
        log_status "Stopping IIS site..."
        powershell -ExecutionPolicy Bypass -File "$SCRIPT_DIR/setup_iis_full.ps1" stop
    fi
}

# Run pytest tests locally
run_tests_local() {
    log_status "Running pytest tests..."

    # Build pytest command
    local pytest_args="-v"

    if [[ -n "$TEST_FILTER" ]]; then
        pytest_args="$pytest_args -k $TEST_FILTER"
    fi

    if [[ "$VERBOSE" == "true" ]]; then
        pytest_args="$pytest_args -s"
    fi

    # Set environment variables for tests
    # IMPORTANT: MSYS_NO_PATHCONV=1 prevents MSYS from converting paths like
    # /mod_pagespeed_example to C:/Program Files/Git/mod_pagespeed_example
    export MSYS_NO_PATHCONV=1
    export PAGESPEED_HOST=localhost
    export PAGESPEED_PORT=$IIS_PORT
    export PAGESPEED_TEST_ROOT=/mod_pagespeed_test
    export PAGESPEED_EXAMPLE_ROOT=/mod_pagespeed_example
    export PAGESPEED_CACHE_DIR='C:\pagespeed_cache'
    export PAGESPEED_SERVER_TYPE=iis

    if [[ "$NO_MODULE" == "true" ]]; then
        export PAGESPEED_STATS_ENABLED=0
    else
        export PAGESPEED_STATS_ENABLED=1
    fi

    log_status "Running: pytest automatic/ iis/ $pytest_args"
    cd "$SCRIPT_DIR"
    python -m pytest automatic/ iis/ $pytest_args
}

# Main execution for local mode
main_local() {
    echo ""
    echo "========================================================================"
    echo " PageSpeed IIS Integration Tests (Local Windows)"
    echo "========================================================================"
    echo ""

    if [[ "$USE_IIS_EXPRESS" == "true" ]]; then
        log_status "Mode: IIS Express (legacy)"
    else
        log_status "Mode: Full IIS (Windows Server)"
    fi

    if [[ "$NO_MODULE" == "true" ]]; then
        log_status "PageSpeed Module: Disabled (baseline test)"
    else
        log_status "PageSpeed Module: Enabled"
    fi

    echo ""

    # Setup IIS unless skipping
    if [[ "$SKIP_BUILD" != "true" ]]; then
        setup_iis_local
    else
        log_status "Skipping IIS setup (--skip-build)"
    fi

    # Exit early if build-only
    if [[ "$BUILD_ONLY" == "true" ]]; then
        log_success "IIS setup complete (--build-only)"
        exit 0
    fi

    # Run tests
    local test_result=0
    run_tests_local || test_result=$?

    # Stop IIS unless keeping running
    if [[ "$KEEP_RUNNING" != "true" ]]; then
        stop_iis_local
    else
        log_status "Keeping IIS running (--keep-running)"
    fi

    # Report results
    echo ""
    if [[ $test_result -eq 0 ]]; then
        log_success "========================================"
        log_success " All Tests Passed!"
        log_success "========================================"
    else
        log_error "========================================"
        log_error " Some Tests Failed (exit code: $test_result)"
        log_error "========================================"
    fi

    exit $test_result
}

# ===== REMOTE EXECUTION FUNCTIONS (for SSH to Windows VM) =====

# Determine SSH command based on available auth methods
setup_ssh() {
    # SSH options
    SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10"

    # Prefer SSH key authentication if SSH_AUTH_SOCK is set
    if [[ -n "$SSH_AUTH_SOCK" ]]; then
        log_status "Using SSH key authentication"
        SSH_CMD="ssh $SSH_OPTS -p $WINDOWS_SSH_PORT"
        SCP_CMD="scp $SSH_OPTS -P $WINDOWS_SSH_PORT"
    elif command -v sshpass &> /dev/null; then
        log_status "Using password authentication (sshpass)"
        SSH_CMD="sshpass -p '$WINDOWS_PASSWORD' ssh $SSH_OPTS -p $WINDOWS_SSH_PORT"
        SCP_CMD="sshpass -p '$WINDOWS_PASSWORD' scp $SSH_OPTS -P $WINDOWS_SSH_PORT"
    else
        log_warning "No SSH_AUTH_SOCK set and sshpass not found"
        log_warning "You may need to enter password manually"
        log_warning "Set SSH_AUTH_SOCK or install sshpass for non-interactive operation"
        SSH_CMD="ssh $SSH_OPTS -p $WINDOWS_SSH_PORT"
        SCP_CMD="scp $SSH_OPTS -P $WINDOWS_SSH_PORT"
    fi
}

# Execute command on Windows via SSH
run_on_windows() {
    local cmd="$1"
    eval $SSH_CMD "$WINDOWS_USER@$WINDOWS_HOST" "powershell -Command \"$cmd\""
}

# Copy file to Windows
copy_to_windows() {
    local src="$1"
    local dest="$2"
    eval $SCP_CMD "$src" "$WINDOWS_USER@$WINDOWS_HOST:$dest"
}

# Check if Windows VM is reachable
check_windows_vm() {
    log_status "Checking Windows VM connectivity..."

    if ! eval $SSH_CMD "$WINDOWS_USER@$WINDOWS_HOST" "echo 'Connected'" &> /dev/null; then
        log_error "Cannot connect to Windows VM at $WINDOWS_HOST:$WINDOWS_SSH_PORT"
        log_error ""
        log_error "Make sure the dockur/windows container is running:"
        log_error "  docker compose --profile windows-x64 up -d"
        log_error "  ./windows-dev/wait-for-windows.sh"
        log_error ""
        if [[ -z "$SSH_AUTH_SOCK" ]]; then
            log_error "SSH key auth: Set SSH_AUTH_SOCK to your SSH agent socket"
            log_error "  export SSH_AUTH_SOCK=/tmp/ssh-XXX/agent.123"
        fi
        exit 1
    fi

    log_success "Connected to Windows VM"
}

# Check if source is accessible on Windows
check_source_mount() {
    log_status "Checking source access on Windows..."

    local mount_check
    mount_check=$(run_on_windows "if (Test-Path 'C:\\pagespeed\\install\\mod_pagespeed_example') { 'ok' } else { 'missing' }")

    if [[ "$mount_check" == *"missing"* ]]; then
        log_warning "Source not accessible at C:\\pagespeed"
        log_warning "The test content directory install\\mod_pagespeed_example is missing"
        log_warning ""
        log_warning "Make sure the SMB share is working:"
        log_warning "  Test: ssh -p $WINDOWS_SSH_PORT $WINDOWS_USER@$WINDOWS_HOST 'dir \\\\host.lan\\Data'"
        log_warning ""
        log_warning "Or copy source manually (slow):"
        log_warning "  scp -r -P $WINDOWS_SSH_PORT $SOURCE_ROOT $WINDOWS_USER@$WINDOWS_HOST:C:\\pagespeed"
        return 1
    fi

    log_success "Source accessible at C:\\pagespeed"
    return 0
}

# Copy test scripts to Windows (always do this to ensure latest versions)
sync_test_scripts() {
    log_status "Syncing test scripts to Windows..."

    # Ensure directories exist
    run_on_windows "New-Item -ItemType Directory -Path 'C:\\pagespeed\\test\\system' -Force | Out-Null"

    # Copy setup scripts
    copy_to_windows "$SCRIPT_DIR/setup_iis_full.ps1" "C:\\pagespeed\\test\\system\\setup_iis_full.ps1"
    copy_to_windows "$SCRIPT_DIR/setup_iis_test.ps1" "C:\\pagespeed\\test\\system\\setup_iis_test.ps1"
    copy_to_windows "$SCRIPT_DIR/install_pagespeed_module.ps1" "C:\\pagespeed\\test\\system\\install_pagespeed_module.ps1"

    log_success "Test scripts synced"
}

# Setup IIS on Windows
setup_iis() {
    if [[ "$USE_IIS_EXPRESS" == "true" ]]; then
        log_status "Setting up IIS Express..."
        local setup_script="setup_iis_test.ps1"
        local action="start"
    else
        log_status "Setting up Full IIS..."
        local setup_script="setup_iis_full.ps1"
        local action="install"
    fi

    local module_flag=""
    if [[ "$NO_MODULE" == "true" ]]; then
        module_flag="-NoModule"
    fi

    log_status "Running: $setup_script $action -Port $IIS_PORT $module_flag"
    run_on_windows "cd C:\\pagespeed\\test\\system; .\\$setup_script $action -Port $IIS_PORT $module_flag"

    # Verify server is responding
    log_status "Verifying IIS is responding..."
    local attempts=0
    local max_attempts=10
    while [[ $attempts -lt $max_attempts ]]; do
        if curl -s -o /dev/null -w "%{http_code}" "http://$WINDOWS_HOST:$IIS_PORT/" 2>/dev/null | grep -q "200"; then
            log_success "IIS is responding on port $IIS_PORT"
            return 0
        fi
        attempts=$((attempts + 1))
        sleep 2
    done

    log_error "IIS is not responding after $max_attempts attempts"
    return 1
}

# Stop IIS on Windows
stop_iis() {
    if [[ "$USE_IIS_EXPRESS" == "true" ]]; then
        log_status "Stopping IIS Express..."
        run_on_windows "cd C:\\pagespeed\\test\\system; .\\setup_iis_test.ps1 stop"
    else
        log_status "Stopping IIS site..."
        run_on_windows "cd C:\\pagespeed\\test\\system; .\\setup_iis_full.ps1 stop"
    fi
}

# Install Python dependencies on Windows
install_dependencies() {
    log_status "Installing Python dependencies..."
    run_on_windows "cd C:\\pagespeed\\test\\system; python -m pip install -r requirements.txt -q 2>&1"
}

# Run pytest tests
run_tests() {
    log_status "Running pytest tests..."

    # Build pytest command
    local pytest_args="-v"

    if [[ -n "$TEST_FILTER" ]]; then
        pytest_args="$pytest_args -k '$TEST_FILTER'"
    fi

    if [[ "$VERBOSE" == "true" ]]; then
        pytest_args="$pytest_args -s"
    fi

    # Set environment variables for tests
    local env_vars="
        \$env:PAGESPEED_HOST = 'localhost';
        \$env:PAGESPEED_PORT = '$IIS_PORT';
        \$env:PAGESPEED_TEST_ROOT = '/mod_pagespeed_test';
        \$env:PAGESPEED_EXAMPLE_ROOT = '/mod_pagespeed_example';
        \$env:PAGESPEED_CACHE_DIR = 'C:\\pagespeed_cache';
        \$env:PAGESPEED_SERVER_TYPE = 'iis';
    "

    if [[ "$NO_MODULE" == "true" ]]; then
        env_vars="$env_vars \$env:PAGESPEED_STATS_ENABLED = '0';"
    else
        env_vars="$env_vars \$env:PAGESPEED_STATS_ENABLED = '1';"
    fi

    log_status "Running: pytest automatic/ iis/ $pytest_args"
    run_on_windows "$env_vars cd C:\\pagespeed\\test\\system; python -m pytest automatic/ iis/ $pytest_args"
}

# Main execution
main() {
    echo ""
    echo "========================================================================"
    echo " PageSpeed IIS Integration Tests (via Windows VM)"
    echo "========================================================================"
    echo ""

    if [[ "$USE_IIS_EXPRESS" == "true" ]]; then
        log_status "Mode: IIS Express (legacy)"
    else
        log_status "Mode: Full IIS (Windows Server)"
    fi

    if [[ "$NO_MODULE" == "true" ]]; then
        log_status "PageSpeed Module: Disabled (baseline test)"
    else
        log_status "PageSpeed Module: Enabled"
    fi

    echo ""

    setup_ssh
    check_windows_vm

    # Always sync test scripts to ensure latest versions
    sync_test_scripts

    # Check source access
    if ! check_source_mount; then
        log_error "Cannot proceed without source access"
        exit 1
    fi

    # Setup IIS unless skipping
    if [[ "$SKIP_BUILD" != "true" ]]; then
        setup_iis
    else
        log_status "Skipping IIS setup (--skip-build)"
    fi

    # Exit early if build-only
    if [[ "$BUILD_ONLY" == "true" ]]; then
        log_success "IIS setup complete (--build-only)"
        exit 0
    fi

    # Install Python dependencies
    install_dependencies

    # Run tests
    local test_result=0
    run_tests || test_result=$?

    # Stop IIS unless keeping running
    if [[ "$KEEP_RUNNING" != "true" ]]; then
        stop_iis
    else
        log_status "Keeping IIS running (--keep-running)"
    fi

    # Report results
    echo ""
    if [[ $test_result -eq 0 ]]; then
        log_success "========================================"
        log_success " All Tests Passed!"
        log_success "========================================"
    else
        log_error "========================================"
        log_error " Some Tests Failed (exit code: $test_result)"
        log_error "========================================"
    fi

    exit $test_result
}

# Dispatch to appropriate execution mode
if [[ "$LOCAL_MODE" == "true" ]]; then
    main_local
else
    main
fi
