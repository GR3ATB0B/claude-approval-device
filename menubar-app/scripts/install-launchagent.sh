#!/usr/bin/env bash
# Install the Claude Approver as a LaunchAgent so it auto-starts on login.
# Assumes "Claude Approver.app" is already in /Applications.
#
# Run:    ./scripts/install-launchagent.sh
# Remove: ./scripts/install-launchagent.sh --uninstall

set -euo pipefail

LABEL="com.gr3atb0b.claudeapprover"
PLIST="${HOME}/Library/LaunchAgents/${LABEL}.plist"
APP_PATH="/Applications/Claude Approver.app/Contents/MacOS/ClaudeApprover"

if [[ "${1:-}" == "--uninstall" ]]; then
    echo "==> removing LaunchAgent"
    launchctl unload "${PLIST}" 2>/dev/null || true
    rm -f "${PLIST}"
    echo "Removed."
    exit 0
fi

if [[ ! -x "${APP_PATH}" ]]; then
    echo "Error: ${APP_PATH} not found. Build and install first:" >&2
    echo "  ./scripts/build-app.sh && cp -R 'build/Claude Approver.app' /Applications/" >&2
    exit 1
fi

mkdir -p "${HOME}/Library/LaunchAgents"
cat > "${PLIST}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>${APP_PATH}</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>${HOME}/Library/Logs/ClaudeApprover.log</string>
    <key>StandardErrorPath</key>
    <string>${HOME}/Library/Logs/ClaudeApprover.log</string>
</dict>
</plist>
EOF

launchctl unload "${PLIST}" 2>/dev/null || true
launchctl load "${PLIST}"

echo "Installed: ${PLIST}"
echo "Logs:      ${HOME}/Library/Logs/ClaudeApprover.log"
