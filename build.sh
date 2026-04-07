#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "=== Orb Browser Build Script ==="

# Check CEF
if [ ! -d "third_party/cef" ]; then
    echo ""
    echo "CEF binary distribution not found. Downloading..."
    echo ""

    CEF_VERSION="146.0.10+g8219561+chromium-146.0.7680.179"
    CEF_URL="https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION}_linux64.tar.bz2"

    mkdir -p third_party
    cd third_party

    echo "Downloading CEF ($CEF_VERSION)..."
    if command -v wget &>/dev/null; then
        wget -q --show-progress "$CEF_URL" -O cef.tar.bz2
    elif command -v curl &>/dev/null; then
        curl -L --progress-bar "$CEF_URL" -o cef.tar.bz2
    else
        echo "Error: wget or curl required"
        exit 1
    fi

    echo "Extracting..."
    tar xf cef.tar.bz2
    mv cef_binary_*_linux64 cef
    rm cef.tar.bz2

    echo "CEF downloaded successfully."
    cd ..
fi

# Download filter lists if not present
if [ ! -s "resources/easylist.txt" ] || head -1 resources/easylist.txt | grep -q "^#"; then
    echo "Downloading EasyList..."
    curl -sL "https://easylist.to/easylist/easylist.txt" -o resources/easylist.txt
fi

if [ ! -s "resources/easyprivacy.txt" ] || head -1 resources/easyprivacy.txt | grep -q "^#"; then
    echo "Downloading EasyPrivacy..."
    curl -sL "https://easylist.to/easylist/easyprivacy.txt" -o resources/easyprivacy.txt
fi

# Build
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo ""
echo "Build complete! Run with: cd build && ./orb-browser"
