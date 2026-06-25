#include "FarmService.h"

namespace rdws::farm {

std::vector<Farm> FarmService::findAll() {
    return repo_.findAll();
}

std::optional<Farm> FarmService::findById(const std::string& id) {
    return repo_.findById(id);
}

std::string FarmService::create(const FarmCreate& data) {
    return repo_.create(data);
}

bool FarmService::update(const std::string& id, const FarmUpdate& data) {
    return repo_.update(id, data);
}

bool FarmService::remove(const std::string& id) {
    return repo_.remove(id);
}

} // namespace rdws::farm
