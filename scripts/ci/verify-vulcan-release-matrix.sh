#!/usr/bin/env bash
# Verify the complete dedicated release matrix and emit its checksum file.
# 验证完整的专用发布矩阵并生成校验和文件。

set -euo pipefail

# Keep the expected host matrix explicit so missing or extra artifacts fail closed.
# 显式维护预期宿主矩阵，缺失或多余资产都必须失败。
EXPECTED=(
    vulcan-codebase-memory-mcp-linux-x86_64.tar.gz
    vulcan-codebase-memory-mcp-linux-aarch64.tar.gz
    vulcan-codebase-memory-mcp-macos-x86_64.tar.gz
    vulcan-codebase-memory-mcp-macos-aarch64.tar.gz
    vulcan-codebase-memory-mcp-windows-x86_64.zip
)

for artifact in "${EXPECTED[@]}"; do
    test -s "$artifact"
done
test "$(find . -maxdepth 1 -type f \( -name '*.tar.gz' -o -name '*.zip' \) | wc -l)" -eq "${#EXPECTED[@]}"
sha256sum "${EXPECTED[@]}" > checksums.txt
