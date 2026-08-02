#!/usr/bin/env bash
# Package one dedicated Vulcan runtime with a verifiable manifest.
# 打包一个带可验证清单的 Vulcan 专用运行时。

set -euo pipefail

# Compute a SHA-256 digest with the native tool available on the runner.
# 使用运行器原生提供的工具计算 SHA-256 摘要。
sha256_file() {
    local file_path="$1"

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file_path" | awk '{print $1}'
        return
    fi

    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$file_path" | awk '{print $1}'
        return
    fi

    echo "No SHA-256 command is available on this runner." >&2
    return 127
}

if [ "$#" -ne 2 ]; then
    echo "Usage: scripts/ci/package-vulcan-runtime.sh <platform> <version>" >&2
    exit 2
fi

# Normalize the public version while preserving the caller-provided platform.
# 规范化对外版本号，同时保留调用方指定的平台。
PLATFORM="$1"
VERSION="${2#v}"
PACKAGE_DIR="package-${PLATFORM}"

case "$PLATFORM" in
windows-*)
    EXECUTABLE="vulcan-codebase-memory-mcp.exe"
    ARCHIVE="vulcan-codebase-memory-mcp-${PLATFORM}.zip"
    ;;
*)
    EXECUTABLE="vulcan-codebase-memory-mcp"
    ARCHIVE="vulcan-codebase-memory-mcp-${PLATFORM}.tar.gz"
    ;;
esac

# Stage only the runtime and its contract manifest into a fresh package root.
# 仅将运行时和契约清单放入全新的打包目录。
rm -rf "$PACKAGE_DIR" "$ARCHIVE"
mkdir "$PACKAGE_DIR"
cp "build/c/$EXECUTABLE" "$PACKAGE_DIR/$EXECUTABLE"
SHA256=$(sha256_file "$PACKAGE_DIR/$EXECUTABLE")
printf '{\n  "protocol": "vulcan.codebase-memory/2",\n  "version": "%s",\n  "platform": "%s",\n  "executable": "%s",\n  "sha256": "%s"\n}\n' \
    "$VERSION" "$PLATFORM" "$EXECUTABLE" "$SHA256" > "$PACKAGE_DIR/manifest.json"

if [[ "$PLATFORM" == windows-* ]]; then
    (cd "$PACKAGE_DIR" && zip -q -r "../$ARCHIVE" "$EXECUTABLE" manifest.json)
else
    tar -czf "$ARCHIVE" -C "$PACKAGE_DIR" "$EXECUTABLE" manifest.json
fi
