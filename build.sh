#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "=== Zen Browser Build Script ==="

# Check CEF
if [ ! -d "third_party/cef" ]; then
    echo ""
    echo "CEF binary distribution not found!"
    echo ""
    echo "Download it from: https://cef-builds.spotifycdn.com/index.html"
    echo "  - Select 'Linux 64-bit' -> 'Standard Distribution'"
    echo "  - Extract and rename to third_party/cef/"
    echo ""
    echo "Example:"
    echo "  wget 'https://cef-builds.spotifycdn.com/cef_binary_<VERSION>_linux64.tar.bz2'"
    echo "  tar xf cef_binary_*_linux64.tar.bz2"
    echo "  mv cef_binary_*_linux64 third_party/cef"
    exit 1
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
echo "Build complete! Run with: cd build && ./zen-browser"
