// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

// What a service outside this repository starts with: an identity and a broker client. It
// doesn't connect (no gateway in CI); building and linking it is the test.
#include "broker/ServiceClient.h"
#include "rdws_version.h"

#include <iostream>

int main() {
  servicegateway::ServiceIdentity identity;
  identity.serviceName = "hello_service";
  identity.serviceId = "hello_001";
  identity.capabilities = {"hello.say"};

  const servicegateway::ServiceClient client(identity, "unix:///tmp/rdws_consumer_smoke.sock");
  std::cout << "rdws_us " << rdws::kVersion << " (API " << rdws::kApiVersion << "): "
            << identity.serviceName << " ready, connected=" << std::boolalpha
            << client.isConnected() << '\n';
  return client.isConnected() ? 1 : 0; // nothing to connect to
}
