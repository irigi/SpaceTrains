#pragma once

#include <filesystem>

#include "domain/Types.hpp"

namespace spacetrains::data_loader {

class DataLoader {
public:
    [[nodiscard]] domain::UniverseDefinition load_universe(const std::filesystem::path& root) const;
};

}  // namespace spacetrains::data_loader
