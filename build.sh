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

# Install Widevine DRM into user cache (CEF expects versioned path)
CACHE_DIR="$HOME/.cache/orb-browser/WidevineCdm"
for wv_path in /usr/lib/chromium/WidevineCdm /usr/lib64/chromium/WidevineCdm /opt/google/chrome/WidevineCdm; do
    if [ -d "$wv_path" ] && [ -f "$wv_path/manifest.json" ]; then
        WV_VER=$(python3 -c "import json; print(json.load(open('$wv_path/manifest.json'))['version'])" 2>/dev/null)
        if [ -n "$WV_VER" ] && [ ! -d "$CACHE_DIR/$WV_VER" ]; then
            mkdir -p "$CACHE_DIR/$WV_VER"
            cp -r "$wv_path"/* "$CACHE_DIR/$WV_VER/"
            echo "{\"Path\":\"$CACHE_DIR/$WV_VER\"}" > "$CACHE_DIR/latest-component-updated-widevine-cdm"
            echo "Widevine CDM $WV_VER installed to cache"
        fi
        break
    fi
done

echo ""
echo "Build complete! Run with: cd build && ./orb-browser"
