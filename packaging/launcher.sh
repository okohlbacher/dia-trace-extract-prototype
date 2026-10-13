#!/bin/sh
# DIATraceExtractor launcher of the binary release: the bundled libraries and OpenMS's shared data are found
# relative to this script, so the directory can live anywhere. The tool itself is libexec/DIATraceExtractor.
here=$(cd "$(dirname "$0")/.." && pwd)
export OPENMS_DATA_PATH="$here/share/OpenMS"
# Linux: the bundled libraries via the loader path. macOS: the tool finds them through its own rpath
# (@executable_path/../lib, ci/macos_relocate_sign.sh); DYLD_* variables would be ignored by the hardened runtime.
[ "$(uname -s)" = Darwin ] || export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# idle OpenMP threads sleep instead of spinning (the tool sets this itself on Linux; here it saves the re-exec)
[ -n "$OMP_WAIT_POLICY" ] || export OMP_WAIT_POLICY=PASSIVE
exec "$here/libexec/DIATraceExtractor" "$@"
