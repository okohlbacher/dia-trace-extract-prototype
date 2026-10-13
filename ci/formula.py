#!/usr/bin/env python3
"""Write the Homebrew formula for a release from the macOS tarball.
usage: formula.py VERSION TARBALL_URL TARBALL_PATH OUT.rb"""
import hashlib, sys
version, url, path, out = sys.argv[1:5]
sha = hashlib.sha256(open(path, "rb").read()).hexdigest()
open(out, "w").write(f'''# Homebrew formula for DIATraceExtractor {version}: installs the binary release (the tool, the OpenMS and
# vcpkg libraries it links, OpenMS's shared data) under the keg and links the launcher into bin.
class Diatraceextractor < Formula
  desc "DIA fragment and MS1 isotope trace extraction from predicted spectral libraries (OpenMS TOPP tool)"
  homepage "https://github.com/okohlbacher/dia-trace-extract-prototype"
  url "{url}"
  sha256 "{sha}"
  version "{version}"

  depends_on arch: :arm64
  depends_on macos: :sequoia

  def install
    libexec.install Dir["*"]
    bin.write_exec_script libexec/"bin/DIATraceExtractor"
  end

  test do
    assert_match "Version: {version}", shell_output("#{{bin}}/DIATraceExtractor --help 2>&1")
  end
end
''')
print("->", out, sha)
