// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

// ServiceClient against a fake broker that acknowledges the registration and then either
// answers the PINGs or goes silent without closing the connection, like a hung gateway or a
// half-open TCP connection: the client must reconnect by itself in the second case only.

#include "broker/ServiceClient.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using servicegateway::ServiceClient;
using servicegateway::ServiceIdentity;
using namespace std::chrono_literals;

namespace {

class FakeBroker {
public:
  FakeBroker(std::string path, bool answerPings) : path_(std::move(path)), answerPings_(answerPings) {
    ::unlink(path_.c_str());
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listenFd_, 4) != 0) {
      ADD_FAILURE() << "fake broker could not listen on " << path_;
    }
    acceptThread_ = std::thread([this] { acceptLoop(); });
  }

  ~FakeBroker() {
    stopping_ = true;
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    {
      std::scoped_lock lock(mutex_);
      for (const int fd : clients_) {
        ::shutdown(fd, SHUT_RDWR);
      }
    }
    if (acceptThread_.joinable()) {
      acceptThread_.join();
    }
    for (auto& t : clientThreads_) {
      t.join();
    }
    for (const int fd : clients_) {
      ::close(fd);
    }
    ::unlink(path_.c_str());
  }

  FakeBroker(const FakeBroker&) = delete;
  FakeBroker& operator=(const FakeBroker&) = delete;

  [[nodiscard]] int connections() const { return connections_.load(); }

private:
  std::string path_;
  bool answerPings_;
  int listenFd_ = -1;
  std::atomic<bool> stopping_{false};
  std::atomic<int> connections_{0};
  std::thread acceptThread_;
  std::mutex mutex_;
  std::vector<int> clients_;
  std::vector<std::thread> clientThreads_;

  void acceptLoop() {
    while (!stopping_) {
      const int fd = ::accept(listenFd_, nullptr, nullptr);
      if (fd < 0) {
        return;
      }
      ++connections_;
      std::scoped_lock lock(mutex_);
      clients_.push_back(fd);
      clientThreads_.emplace_back([this, fd] { serve(fd); });
    }
  }

  // Answers the IDENTIFY, then the PINGs only if answerPings_. Never closes the connection
  // itself: a silent broker keeps it open, as a hung gateway would.
  void serve(int fd) {
    char buffer[4096];
    std::string pending;
    while (!stopping_) {
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0) {
        return;
      }
      pending.append(buffer, static_cast<size_t>(n));
      std::string::size_type pos;
      while ((pos = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, pos);
        pending.erase(0, pos + 1);
        if (line.find("\"IDENTIFY\"") != std::string::npos) {
          send(fd, R"({"type":"ACKNOWLEDGED","serviceId":"heartbeat_test","status":"registered"})");
        } else if (answerPings_ && line.find("\"PING\"") != std::string::npos) {
          send(fd, R"({"type":"PONG"})");
        }
      }
    }
  }

  static void send(int fd, const std::string& message) {
    const std::string framed = message + "\n";
    (void)::send(fd, framed.data(), framed.size(), MSG_NOSIGNAL);
  }
};

ServiceIdentity identity() {
  ServiceIdentity id;
  id.serviceName = "heartbeat_test";
  id.serviceId = "heartbeat_test";
  id.capabilities = {"heartbeat.test"};
  return id;
}

std::string socketPath(const char* name) {
  return "/tmp/rdws_heartbeat_" + std::string(name) + "_" + std::to_string(::getpid()) + ".sock";
}

// Runs the client on its own thread until the scope ends
struct RunningClient {
  ServiceClient client;
  std::thread thread;

  RunningClient(const std::string& path, std::chrono::milliseconds ping,
                std::chrono::milliseconds deadAfter)
      : client(identity(), "unix://" + path) {
    client.setHeartbeat(ping, deadAfter);
    thread = std::thread([this] { client.run(); });
  }
  ~RunningClient() {
    client.stop();
    if (thread.joinable()) {
      thread.join();
    }
  }
  RunningClient(const RunningClient&) = delete;
  RunningClient& operator=(const RunningClient&) = delete;
};

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return pred();
}

} // namespace

TEST(ServiceClientHeartbeat, SilentBrokerIsDroppedAndTheClientReconnects) {
  const auto path = socketPath("silent");
  FakeBroker broker(path, /*answerPings=*/false);
  RunningClient running(path, /*ping=*/100ms, /*deadAfter=*/400ms);

  ASSERT_TRUE(waitFor([&] { return running.client.isRegistered(); }, 2s));
  // No PONG, no close: after deadAfter of silence the client drops the connection and run()
  // reconnects (after its 1 s initial backoff)
  EXPECT_TRUE(waitFor([&] { return broker.connections() >= 2; }, 4s))
      << "connections: " << broker.connections();
}

TEST(ServiceClientHeartbeat, AnsweredPingsKeepTheConnection) {
  const auto path = socketPath("answered");
  FakeBroker broker(path, /*answerPings=*/true);
  RunningClient running(path, /*ping=*/100ms, /*deadAfter=*/400ms);

  ASSERT_TRUE(waitFor([&] { return running.client.isRegistered(); }, 2s));
  // Well past deadAfter: the PONGs keep it alive, so no reconnection
  std::this_thread::sleep_for(1500ms);
  EXPECT_EQ(broker.connections(), 1);
  EXPECT_TRUE(running.client.isConnected());
}
