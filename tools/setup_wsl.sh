#!/usr/bin/env bash
# Setup WSL2 (Ubuntu 24.04/26.04) work env for the Astral Chain Linux port.
# Run inside WSL:  bash tools/setup_wsl.sh
# The repo must live on ext4 (~/src), not /mnt/c, for build speed.
set -euo pipefail

sudo apt update
sudo apt install -y clang lld ninja-build make pkg-config \
  libsdl3-dev libopus-dev libvpx-dev libzstd-dev \
  libvulkan-dev vulkan-validationlayers vulkan-tools \
  glslc libshaderc-dev spirv-tools git python3-venv dotnet-sdk-10.0

# Python env (3.12 like Windows .venv; extraction runs under it with zstandard)
if [ ! -d .venv ]; then
  python3 -m venv .venv
fi
.venv/bin/pip install -r tools/requirements.txt

echo "--- toolchain ---"
clang --version | head -n 2
ninja --version
echo "--- vulkan (needs /dev/dxg + Intel Hasvk/gfxstream ICD) ---"
vulkaninfo --summary | head -n 40
echo "setup_wsl: ok"
