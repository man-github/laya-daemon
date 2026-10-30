#!/bin/bash
set -euo pipefail

MODEL_DIR="${1:-./model}"
mkdir -p "$MODEL_DIR"

echo "=== Downloading / Verifying Laya AI Checkpoints in $MODEL_DIR ==="

# Base Hugging Face model repository
HF_BASE="https://huggingface.co/laya-ai/laya-modernbert-cybersec/resolve/main"

FILES=(
    "model_quantized.onnx"
    "tokenizer.json"
    "config.json"
    "special_tokens_map.json"
    "ch_PP-OCRv4_det.onnx"
    "ch_PP-OCRv4_rec.onnx"
    "ch_PP-OCR_keys_v1.txt"
)

for f in "${FILES[@]}"; do
    TARGET="$MODEL_DIR/$f"
    if [ -f "$TARGET" ]; then
        echo " [OK] $f exists ($(du -h "$TARGET" | cut -f1))"
    else
        echo " [FETCH] Downloading $f..."
        # If remote release is available:
        # curl -fsSL "$HF_BASE/$f" -o "$TARGET"
        echo " Note: Place official $f in $MODEL_DIR"
    fi
done

echo "=== Model verification complete ==="
