#!/usr/bin/env bash
# Put the release's Homebrew formula into the tap okohlbacher/homebrew-odia (Formula/diatraceextractor.rb) so that
#   brew tap okohlbacher/odia && brew install diatraceextractor
# installs the latest release. Run after the release workflow published its assets.
# usage: publish_formula.sh TAG [TAP_URL]     (needs git credentials for the tap; GIT_ASKPASS is honoured)
set -euo pipefail
TAG=$1; TAP=${2:-https://github.com/okohlbacher/homebrew-odia.git}
REPO=okohlbacher/dia-trace-extract-prototype
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
ASSET="https://github.com/$REPO/releases/download/$TAG/diatraceextractor.rb"
echo "fetching $ASSET"
if command -v gh > /dev/null; then gh release download "$TAG" --repo "$REPO" --pattern diatraceextractor.rb --dir "$W"
else curl -fsSL -o "$W/diatraceextractor.rb" ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} "$ASSET"; fi
git clone -q "$TAP" "$W/tap"
mkdir -p "$W/tap/Formula"
cp "$W/diatraceextractor.rb" "$W/tap/Formula/diatraceextractor.rb"
cd "$W/tap"
git add Formula/diatraceextractor.rb
git -c user.name="${GIT_AUTHOR_NAME:-release}" -c user.email="${GIT_AUTHOR_EMAIL:-release@localhost}" commit -q -m "diatraceextractor $TAG" || { echo "formula unchanged"; exit 0; }
git push -q
echo "tap updated: $TAP ($TAG)"
