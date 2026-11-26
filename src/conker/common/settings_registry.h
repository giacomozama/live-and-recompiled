#pragma once
#include <span>

#include "app/app.h"

namespace settings {

// every setting that is one value (app::GameInfo::settings)
std::span<const app::Setting> Registry();

}  // namespace settings
