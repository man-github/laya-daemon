#!/bin/bash
set -euo pipefail

echo "=== Installing Laya AI Build & Runtime Dependencies ==="

sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libonnxruntime-dev \
    libpoppler-cpp-dev \
    libre2-dev \
    libarchive-dev \
    libtinyxml2-dev \
    curl \
    gnupg \
    systemd

echo "=== Dependencies installed successfully! ==="
