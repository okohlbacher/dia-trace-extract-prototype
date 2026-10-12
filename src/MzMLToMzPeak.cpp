// ODIA-V3 helper: write an mzML run as mzPeak (point layout, split metadata) with the mzpeak-openms streaming
// writer. Retention times are passed in seconds (the writer stores minutes, as the format expects); centroid
// spectra go to the peaks table, profile spectra to the data table; the first precursor's isolation window is
// kept per MS2 spectrum.
#include <mzpeak/writer.h>
#include <mzpeak/run_metadata.h>

#include <boost/json.hpp>

#include <OpenMS/FORMAT/DATAACCESS/MSDataTransformingConsumer.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/METADATA/ExperimentalSettings.h>
#include <OpenMS/METADATA/Instrument.h>

#include <iostream>

int main(int argc, char** argv)
{
  if (argc != 3) { std::cerr << "usage: MzMLToMzPeak in.mzML out.mzpeak\n"; return 1; }
  try
  {
    MzPeak::RunArchiveWriter writer(argv[2]);
    OpenMS::MSDataTransformingConsumer consumer;
    consumer.setExperimentalSettingsFunc([&](const OpenMS::ExperimentalSettings& es) {
      // the instrument model, so a reader can tell the instrument class (DIATraceExtractor's tolerance defaults)
      const std::string name = es.getInstrument().getName();
      if (name.empty()) { return; }
      boost::json::object md;
      md["instrument_configuration_list"] = boost::json::array{boost::json::object{{"parameters", boost::json::array{boost::json::object{{"name", name}}}}}};
      writer.set_metadata(MzPeak::RunMetadata(md));
    });
    consumer.setSpectraProcessingFunc([&](OpenMS::MSSpectrum& s) {
      MzPeak::SpectrumData d;
      d.mz.reserve(s.size());
      d.intensity.reserve(s.size());
      for (const auto& p : s) { d.mz.push_back(p.getMZ()); d.intensity.push_back(p.getIntensity()); }
      d.centroid = s.getType(false) != OpenMS::MSSpectrum::SpectrumType::PROFILE;
      d.ms_level = static_cast<std::uint8_t>(s.getMSLevel());
      d.retention_time = s.getRT();
      d.id = s.getNativeID();
      for (const auto& p : s.getPrecursors())
      {
        MzPeak::PrecursorData pd;
        pd.isolation_target_mz = static_cast<float>(p.getMZ());
        pd.isolation_lower_offset = static_cast<float>(p.getIsolationWindowLowerOffset());
        pd.isolation_upper_offset = static_cast<float>(p.getIsolationWindowUpperOffset());
        MzPeak::SelectedIonData ion;
        ion.mz = p.getMZ();
        if (p.getCharge() != 0) { ion.charge = p.getCharge(); }
        pd.selected_ions.push_back(ion);
        d.precursors.push_back(std::move(pd));
      }
      writer.add(std::move(d));
    });
    OpenMS::MzMLFile file;
    file.getOptions().setSkipXMLChecks(true);
    file.transform(argv[1], &consumer, false, false);
    writer.finish();
    std::cout << writer.size() << " spectra -> " << argv[2] << "\n";
  }
  catch (const std::exception& e) { std::cerr << "error: " << e.what() << "\n"; return 1; }
  return 0;
}
