#!/usr/bin/env bash
# Build the menu-bar app as a proper .app bundle and codesign it ad-hoc.
# Run from the menubar-app directory:
#   ./scripts/build-app.sh
# Output: ./build/Claude Approver.app

set -euo pipefail

cd "$(dirname "$0")/.."

APP_NAME="Claude Approver"
APP_BUNDLE="build/${APP_NAME}.app"
BIN_NAME="ClaudeApprover"

echo "==> swift build --configuration release"
swift build --configuration release

BIN_PATH=".build/release/${BIN_NAME}"
if [[ ! -x "${BIN_PATH}" ]]; then
    echo "Error: release binary not found at ${BIN_PATH}" >&2
    exit 1
fi

echo "==> assembling ${APP_BUNDLE}"
rm -rf "${APP_BUNDLE}"
mkdir -p "${APP_BUNDLE}/Contents/MacOS"
mkdir -p "${APP_BUNDLE}/Contents/Resources"
cp "${BIN_PATH}" "${APP_BUNDLE}/Contents/MacOS/${BIN_NAME}"
cp Resources/Info.plist "${APP_BUNDLE}/Contents/Info.plist"

echo "==> codesigning ad-hoc"
ENT=$(mktemp -t claude-approver-entitlements)
trap 'rm -f "${ENT}"' EXIT
cat > "${ENT}" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.app-sandbox</key>
    <false/>
</dict>
</plist>
EOF
codesign --force --deep --sign - --entitlements "${ENT}" "${APP_BUNDLE}"

echo
echo "Built: ${APP_BUNDLE}"
echo
echo "Install:"
echo "  cp -R \"${APP_BUNDLE}\" /Applications/"
echo
echo "Run:"
echo "  open \"/Applications/${APP_NAME}.app\""
