// src/platform/esp32/fuji_config_store_factory.cpp

#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config_defaults.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/core/logging.h"

namespace fujinet::platform {

using fujinet::log::Level;
static constexpr const char* TAG = "platform";

std::unique_ptr<config::FujiConfigStore>
create_fuji_config_store(fs::StorageManager& storage)
{
    // What a fresh device starts from: the profile knows its platform's boot disk.
    const auto defaults = config::default_config_for(build::current_build_profile());

    // Convention: "sd0" = SD card (if mounted), "flash" = internal LittleFS
    auto* sd    = storage.get("sd0");
    auto* flash = storage.get("flash");

    if (!sd && !flash) {
        // Last-ditch: in-memory only? or log and return nullptr.
        FN_LOGE(TAG, "No 'sd0' or 'flash' filesystem registered; config will stay in-memory");
        return std::make_unique<config::YamlFujiConfigStoreFs>(
            nullptr, nullptr, "fujinet.yaml", defaults
        );
    }

    // Normal case: SD preferred, flash as backup mirror
    return std::make_unique<config::YamlFujiConfigStoreFs>(
        sd ? sd : flash,               // primary
        sd && flash ? flash : nullptr, // backup (mirror)
        "fujinet.yaml",
        defaults
    );
}

} // namespace fujinet::platform
