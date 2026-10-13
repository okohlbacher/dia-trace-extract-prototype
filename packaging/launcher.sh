#!/bin/sh
# DIATraceExtractor launcher of the binary release: the bundled libraries and OpenMS's shared data are found
# relative to this script, so the directory can live anywhere. The tool itself is libexec/DIATraceExtractor.
here=$(cd "$(dirname "$0")/.." && pwd)
export OPENMS_DATA_PATH="$here/share/OpenMS"
case "$(uname -s)" in
  Darwin) export DYLD_LIBRARY_PATH="$here/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" ;;
  *)      export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac
# idle OpenMP threads sleep instead of spinning (the tool sets this itself on Linux; here it saves the re-exec)
[ -n "$OMP_WAIT_POLICY" ] || export OMP_WAIT_POLICY=PASSIVE
exec "$here/libexec/DIATraceExtractor" "$@"
