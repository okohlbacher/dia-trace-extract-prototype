#!/usr/bin/env bash
# Package an installed tree (cmake --install ... --prefix STAGE/NAME) for the release:
#   Linux    NAME.tar.gz
#   macOS    NAME.tar.gz and NAME.dmg (drag-and-drop folder image)
#   Windows  NAME.zip
# usage: package.sh STAGE NAME OUT_DIR   (STAGE/NAME is the installed tree)
set -euo pipefail
STAGE=$1; NAME=$2; OUT=$3; mkdir -p "$OUT"
case "$(uname -s)" in
  Darwin)
    (cd "$STAGE" && cmake -E tar czf "$OUT/$NAME.tar.gz" "$NAME")
    rm -f "$OUT/$NAME.dmg"
    # hdiutil fails intermittently with "Resource busy" on the hosted runners (a scanner holding the source); retry
    for attempt in 1 2 3 4 5 6; do
      hdiutil create -volname "$NAME" -srcfolder "$STAGE/$NAME" -ov -format UDZO "$OUT/$NAME.dmg" > /dev/null && break
      [ $attempt -eq 6 ] && { echo "hdiutil failed $attempt times" >&2; exit 1; }
      echo "hdiutil attempt $attempt failed; retrying in $((attempt * 10)) s" >&2; sleep $((attempt * 10))
    done
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    (cd "$STAGE" && cmake -E tar cf "$OUT/$NAME.zip" --format=zip "$NAME")
    ;;
  *)
    (cd "$STAGE" && cmake -E tar czf "$OUT/$NAME.tar.gz" "$NAME")
    ;;
esac
ls -la "$OUT"
