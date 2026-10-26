#include "fujinet/config/fuji_config_defaults.h"

namespace fujinet::config {

FujiConfig default_config_for(const build::BuildProfile& profile)
{
    FujiConfig cfg{};
    if (!profile.defaultBootConfigUri.empty()) {
        cfg.boot.configUri = std::string(profile.defaultBootConfigUri);
    }
    return cfg;
}

} // namespace fujinet::config
