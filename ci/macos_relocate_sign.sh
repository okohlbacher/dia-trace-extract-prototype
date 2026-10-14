#!/usr/bin/env bash
# Make an installed macOS tree (bin/ launcher, libexec/ tool, lib/ dylibs) relocatable and sign it.
#   - every non-system dependency whose file is bundled in lib/ is loaded as @rpath/<name> (the OpenMS build
#     links Homebrew's libomp by absolute path, /opt/homebrew/opt/libomp/lib/libomp.dylib);
#   - the tool gets the rpath @executable_path/../lib, every dylib @loader_path, every dylib id is @rpath/<name>;
#   - nothing outside the tree is referenced afterwards except /usr/lib and /System (checked);
#   - every Mach-O is signed: with MACOS_SIGN_IDENTITY (+ MACOS_KEYCHAIN) using the hardened runtime and a secure
#     timestamp, as notarization requires; without, ad hoc (arm64 needs a valid signature after install_name_tool);
#   - the tool carries ci/entitlements.plist (library validation off): Homebrew re-signs the bundled dylibs ad hoc when
#     it rewrites their install names, and a Developer ID process with library validation refuses to map them.
# The hardened runtime ignores DYLD_* variables, so the tree must resolve its libraries without them.
# usage: macos_relocate_sign.sh TREE
set -euo pipefail
T=${1:?tree}
ENT="$(cd "$(dirname "$0")" && pwd)/entitlements.plist"
LIB="$T/lib"
EXE="$T/libexec/DIATraceExtractor"
[ -x "$EXE" ] || { echo "::error::$EXE missing"; exit 1; }

machos=("$EXE")
while IFS= read -r f; do machos+=("$f"); done < <(find "$LIB" -type f -name '*.dylib' | sort)

deps() { otool -L "$1" | tail -n +2 | awk '{print $1}'; }
rpaths() { otool -l "$1" | awk '/cmd LC_RPATH/{getline; getline; print $2}'; }

for f in "${machos[@]}"; do
  chmod u+w "$f"
  self_id=""
  case "$f" in *.dylib) self_id=$(otool -D "$f" | tail -n +2 | head -1) ;; esac
  while IFS= read -r d; do
    case "$d" in /usr/lib/*|/System/*) continue ;; esac
    [ "$d" = "$self_id" ] && continue
    base=$(basename "$d")
    if [ -f "$LIB/$base" ]; then
      [ "$d" = "@rpath/$base" ] || install_name_tool -change "$d" "@rpath/$base" "$f"
    fi
  done < <(deps "$f")
  case "$f" in
    *.dylib)
      install_name_tool -id "@rpath/$(basename "$f")" "$f"
      rpaths "$f" | grep -qx '@loader_path' || install_name_tool -add_rpath '@loader_path' "$f"
      ;;
    *)
      rpaths "$f" | grep -qx '@executable_path/../lib' || install_name_tool -add_rpath '@executable_path/../lib' "$f"
      ;;
  esac
done

# Nothing may point outside the tree any more.
bad=0
for f in "${machos[@]}"; do
  while IFS= read -r d; do
    case "$d" in
      /usr/lib/*|/System/*) ;;
      @rpath/*) [ -f "$LIB/$(basename "$d")" ] || { echo "::error::$f needs $d, which is not bundled"; bad=1; } ;;
      *) echo "::error::$f still references $d"; bad=1 ;;
    esac
  done < <(deps "$f")
done
[ $bad -eq 0 ] || exit 1

if [ -n "${MACOS_SIGN_IDENTITY:-}" ]; then
  sign() { codesign --force --timestamp --options runtime --keychain "$MACOS_KEYCHAIN" --sign "$MACOS_SIGN_IDENTITY" "$@"; }
  echo "signing with: $MACOS_SIGN_IDENTITY (hardened runtime, timestamp)"
else
  sign() { codesign --force --sign - "$@"; }
  echo "no signing identity: ad-hoc signatures"
fi
# libraries first, the executable last (its signature covers nothing else, but the order matches the dependency order)
for f in "${machos[@]:1}"; do sign "$f"; done
if [ -n "${MACOS_SIGN_IDENTITY:-}" ]; then sign --entitlements "$ENT" "$EXE"; else sign "$EXE"; fi
for f in "${machos[@]}"; do codesign --verify --strict "$f" || { echo "::error::signature of $f does not verify"; exit 1; }; done
codesign -dv --verbose=2 "$EXE" 2>&1 | grep -E "Authority|Timestamp|flags|TeamIdentifier" || true
codesign -d --entitlements - "$EXE" 2>/dev/null | grep -A1 library-validation || true
echo "relocated and signed ${#machos[@]} Mach-O files"
