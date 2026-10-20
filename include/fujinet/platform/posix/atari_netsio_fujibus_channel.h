#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/disk/disk_service.h"

#include <memory>

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::io::Channel>
create_atari_netsio_fujibus_channel(std::unique_ptr<fujinet::io::Channel> udp,
                                    fujinet::disk::DiskService* disks = nullptr);

} // namespace fujinet::platform::posix
