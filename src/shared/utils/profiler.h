#pragma once

#include <chrono>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace rdws::utils {

class Profiler {
public:
  explicit Profiler(std::string name = "Profiler") : name_(std::move(name)) {}

  ~Profiler() { dump(); }

  void start(const std::string& label) {
    entries_[label].startedAt = clock::now();
  }

  void stop(const std::string& label) {
    auto it = entries_.find(label);
    if (it == entries_.end()) {
      return;
    }
    const auto elapsed = clock::now() - it->second.startedAt;
    it->second.totalMs += toMs(elapsed);
    it->second.calls++;
  }

  // RAII guard — para automaticamente ao sair do escopo
  struct ScopedTimer {
    ScopedTimer(Profiler& profiler, std::string label)
        : profiler_(profiler), label_(std::move(label)) {
      profiler_.start(label_);
    }
    ~ScopedTimer() { profiler_.stop(label_); }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

  private:
    Profiler& profiler_;
    std::string label_;
  };

  ScopedTimer scoped(const std::string& label) {
    return ScopedTimer{*this, label};
  }

  void dump(std::ostream& out = std::cout) const {
    if (entries_.empty()) {
      return;
    }

    // Ordena por ordem de inserção não garantida em unordered_map,
    // mas preserva todos os labels registrados
    size_t maxLen = 0;
    for (const auto& [label, _] : entries_) {
      maxLen = std::max(maxLen, label.size());
    }

    out << "[" << name_ << "]\n";
    for (const auto& [label, entry] : entries_) {
      out << "  " << label << std::string(maxLen - label.size(), ' ')
          << "  : " << entry.totalMs << " ms";
      if (entry.calls > 1) {
        out << "  (" << entry.calls << "x, avg "
            << (entry.totalMs / entry.calls) << " ms)";
      }
      out << '\n';
    }
  }

private:
  using clock = std::chrono::steady_clock;

  struct Entry {
    clock::time_point startedAt;
    double totalMs{0.0};
    int calls{0};
  };

  static double toMs(const clock::duration& d) {
    return std::chrono::duration<double, std::milli>(d).count();
  }

  std::string name_;
  std::unordered_map<std::string, Entry> entries_;
};

} // namespace rdws::utils
