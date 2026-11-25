// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "psk_cache.h"

#include "../crypto/credential_cipher.h"
#include "../utils/json_helper.h"

#include <rapidjson/document.h>

#include <cstdint>
#include <utility>

namespace rdws::coap {

namespace json = rdws::utils::json;

namespace {

std::string keyFromHex(const std::string& hex) {
  try {
    const auto bytes = rdws::crypto::fromHex(hex);
    return {bytes.begin(), bytes.end()};
  } catch (const rdws::crypto::CredentialCipherError&) {
    return {};
  }
}

} // namespace

bool PskCache::load(const std::string& envelope) {
  rapidjson::Document doc;
  if (doc.Parse(envelope.c_str()).HasParseError()) {
    return false;
  }
  const auto* list = json::getArray(doc, "data");
  if (list == nullptr) {
    return false;
  }

  auto next = std::make_shared<Snapshot>();
  for (const auto& item : list->GetArray()) {
    const auto identity = json::getString(item, "psk_identity");
    const auto keyHex = json::getString(item, "psk_key");
    const auto deviceId = json::getString(item, "device_id");
    auto key = keyHex ? keyFromHex(*keyHex) : std::string{};
    if (!identity || !deviceId || deviceId->empty() || key.empty()) {
      ++next->skipped;
      continue;
    }
    const auto [it, inserted] = next->entries.emplace(
        *identity,
        Entry{.key = std::move(key),
              .device = {.id = *deviceId,
                         .publicId = json::getString(item, "device_public_id").value_or("")}});
    // The map never moves its entries, and the snapshot doesn't change from here on
    auto& entry = it->second;
    entry.bin = {entry.key.size(), reinterpret_cast<const uint8_t*>(entry.key.data())};
  }

  std::scoped_lock lock(mutex_);
  current_ = std::move(next);
  return true;
}

std::shared_ptr<const PskCache::Snapshot> PskCache::snapshot() const {
  std::scoped_lock lock(mutex_);
  return current_;
}

bool PskCache::loaded() const {
  return snapshot() != nullptr;
}

std::size_t PskCache::size() const {
  const auto snap = snapshot();
  return snap ? snap->entries.size() : 0;
}

std::size_t PskCache::skipped() const {
  const auto snap = snapshot();
  return snap ? snap->skipped : 0;
}

std::optional<PskDevice> PskCache::device(std::string_view identity) const {
  const auto snap = snapshot();
  if (!snap) {
    return std::nullopt;
  }
  const auto it = snap->entries.find(std::string(identity));
  return it == snap->entries.end() ? std::nullopt : std::optional(it->second.device);
}

const coap_bin_const_t* PskCache::keyForHandshake(std::string_view identity) {
  inHandshake_ = snapshot();
  if (!inHandshake_) {
    return nullptr;
  }
  const auto it = inHandshake_->entries.find(std::string(identity));
  return it == inHandshake_->entries.end() ? nullptr : &it->second.bin;
}

} // namespace rdws::coap
