#include "FieldService.h"

namespace rdws::field {

std::vector<Field> FieldService::findAll(const std::string& farmId) {
    return repo_.findAll(farmId);
}

std::optional<Field> FieldService::findById(const std::string& id) {
    return repo_.findById(id);
}

std::string FieldService::create(const FieldCreate& data) {
    return repo_.create(data);
}

bool FieldService::update(const std::string& id, const FieldUpdate& data) {
    return repo_.update(id, data);
}

bool FieldService::remove(const std::string& id) {
    return repo_.remove(id);
}

} // namespace rdws::field
