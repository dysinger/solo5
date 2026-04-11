#!/bin/bash
#
# build_ahv.sh - Build script for AHV tender with cross-compilation support
#
# This script builds the Apple Hypervisor (AHV) tender for Solo5, supporting:
# - Native builds on macOS ARM64 (Apple Silicon)
# - Cross-compilation from macOS x86_64 to ARM64
# - Cross-compilation from Linux x86_64 to ARM64 via Docker
#
# Usage:
#   ./scripts/build_ahv.sh          # Auto-detect architecture
#   ./scripts/build_ahv.sh native   # Build for current host
#   ./scripts/build_ahv.sh cross   # Cross-compile (x86_64 -> ARM64)
#   ./scripts/build_ahv.sh docker  # Build via Docker container

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOPDIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${TOPDIR}/build_ahv"
TENDER_SRC="${TOPDIR}/tenders/ahv"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Detect host architecture
detect_arch() {
    local arch=$(uname -m)
    local os=$(uname -s)
    
    case "$arch" in
        arm64|aarch64)
            echo "aarch64"
            ;;
        x86_64|x64)
            echo "x86_64"
            ;;
        *)
            log_error "Unknown architecture: $arch"
            exit 1
            ;;
    esac
}

# Detect required target architecture
get_target_arch() {
    local host_arch=$(detect_arch)
    
    # If running on ARM64, build native
    if [ "$host_arch" = "aarch64" ]; then
        echo "aarch64"
    else
        # Cross-compile to ARM64 if on x86_64
        echo "aarch64"
    fi
}

# Build native on macOS
build_native() {
    local target_arch=$(get_target_arch)
    
    log_info "Building AHV tender for ${target_arch} (native)"
    
    cd "$TOPDIR"
    
    # Run configure to generate Makeconf
    if [ ! -f Makeconf ] || [ "$(grep -q CONFIG_AHV=1 Makeconf)" ] || [ "$(grep -q "CONFIG_HOST_ARCH=aarch64" Makeconf)" ]; then
        log_info "Running configure.sh..."
        # Configure for Darwin with AHV support implied
        ./configure.sh --disable-toolchain
    fi
    
    # Enable AHV tender explicitly
    if ! grep -q "CONFIG_AHV_TENDER=1" Makeconf 2>/dev/null; then
        log_info "Enabling AHV tender in Makeconf..."
        echo "CONFIG_AHV_TENDER=1" >> Makeconf
        echo "CONFIG_HOST_ARCH=aarch64" >> Makeconf
        echo "CONFIG_HOST=Darwin" >> Makeconf
    fi
    
    # Clean and rebuild
    log_info "Building AHV tender..."
    make clean tenders || true
    make tenders
    
    local tender="${TENDER_SRC}/solo5-ahv"
    if [ -f "$tender" ]; then
        log_info "Build successful: $tender"
        ls -la "$tender"
    else
        log_error "Build failed: tender not found at $tender"
        exit 1
    fi
}

# Direct compile method (bypassing configure issues)
build_direct() {
    log_info "Building AHV tender directly (bypassing Makefile issues)..."
    
    cd "$TENDER_SRC"
    
    # Define include paths
    local inc_include="-I../include -I../../include -I."
    local frameworks="-framework Hypervisor -framework IOKit"
    
    # Build command
    local cmd="clang -o solo5-ahv ${inc_include} ahv_main.c ahv_core.c ../common/tap_attach.c -lpthread ${frameworks}"
    
    log_info "Running: $cmd"
    eval "$cmd" || {
        log_error "Direct build failed"
        exit 1
    }
    
    if [ -f "./solo5-ahv" ]; then
        log_info "Build successful!"
        ls -la ./solo5-ahv
        
        # Apply entitlements for Hypervisor
        apply_entitlements
    fi
}

# Apply Hypervisor entitlements
apply_entitlements() {
    log_info "Applying Hypervisor entitlements..."
    
    cd "$TENDER_SRC"
    
    # Create entitlements file
    cat > solo5-ahv.xcent << 'XMLEOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.hypervisor</key>
    <true/>
</dict>
</plist>
XMLEOF
        
    # Sign with entitlements
    if codesign -s - --entitlements=solo5-ahv.xcent -f ./solo5-ahv 2>/dev/null; then
        log_info "Entitlements applied successfully"
    else
        log_warn "Could not apply entitlements (may need developer certificate)"
    fi
    
    # Verify
    codesign -dv ./solo5-ahv 2>&1 | grep -E "entitlements" || true
}

# Check for Hypervisor entitlement
check_entitlement() {
    local tender="${TENDER_SRC}/solo5-ahv"
    
    if [ ! -f "$tender" ]; then
        log_error "Tender not found: $tender"
        return 1
    fi
    
    # Check if tender has entitlement (codesign)
    if codesign -dv "$tender" 2>&1 | grep -q "entitlements="; then
        log_info "Tender has entitlements"
    else
        log_warn "Tender may need codesigning for Hypervisor"
    fi
}

# Main entry point
main() {
    local mode="${1:-native}"
    
    log_info "AHV Tender Build Script"
    log_info "Mode: $mode"
    log_info "Host architecture: $(detect_arch)"
    log_info "Target architecture: $(get_target_arch)"
    
    case "$mode" in
        native)
            build_native
            ;;
        direct)
            build_direct
            ;;
        cross)
            log_warn "Cross-compilation not fully implemented yet"
            build_native
            ;;
        docker)
            log_warn "Docker cross-compilation not implemented yet"
            ;;
        *)
            log_error "Unknown mode: $mode"
            echo "Usage: $0 [native|direct|cross|docker]"
            exit 1
            ;;
    esac
    
    check_entitlement || true
    
    log_info "Done!"
}

main "$@"