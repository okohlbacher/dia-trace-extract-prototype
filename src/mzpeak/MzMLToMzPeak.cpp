// ODIA-V3 helper: write an mzML run as mzPeak with the vendored OpenMS MzPeakFile writer (layout readable by
// MzPeakFile; mzpeak-convert 0.7.0 writes a newer, incompatible layout).
#include "mzpeak/MzPeakFile.h"

#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/KERNEL/MSExperiment.h>

#include <iostream>

int main(int argc, char** argv)
{
  if (argc != 3) { std::cerr << "usage: MzMLToMzPeak in.mzML out.mzpeak\n"; return 1; }
  try
  {
    OpenMS::PeakMap exp;
    OpenMS::MzMLFile().load(argv[1], exp);
    OpenMS::MzPeakFile().store(argv[2], exp);
    std::cout << exp.size() << " spectra -> " << argv[2] << "\n";
  }
  catch (const std::exception& e) { std::cerr << "error: " << e.what() << "\n"; return 1; }
  return 0;
}
