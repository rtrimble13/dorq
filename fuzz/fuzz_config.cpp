// libFuzzer target: arbitrary bytes as a config file. A bad file must be a
// ConfigError and nothing else; a good one must survive to_toml and back.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "config/config.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  try {
    const dorq::Config config = dorq::parse_config(text, "fuzz.toml", false);
    const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
    if (dorq::config_hash(again) != dorq::config_hash(config)) {
      std::abort();  // to_toml lost or changed a setting
    }
  } catch (const dorq::ConfigError&) {
  }
  return 0;
}
