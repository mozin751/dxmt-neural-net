#pragma once

#include <deque>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>   // std::fprintf

namespace dxmt {

class StutterDetector {
public:
  struct Config {
    size_t window     = 120;   // rolling baseline in frames (~2 s at 60 fps)
    double multiplier = 1.5;   // flag if frame_ms > median * multiplier ...
    double floor_ms   = 4.0;   // ... AND exceeds median by at least this much
    size_t min_frames = 30;    // don't classify until baseline exists
  };

  explicit StutterDetector() {}
  explicit StutterDetector(Config c) : cfg_(c) {}

  // Call once per Present, AFTER the present returns (wall-clock frame time).
  // Returns true when this frame is classified as a stutter.
  bool submit(double frame_ms) {
    bool stutter = false;

    if (history_.size() >= cfg_.min_frames) {
      const double med = rolling_median();
      const double threshold = std::max(med * cfg_.multiplier,
                                        med + cfg_.floor_ms);
      if (frame_ms > threshold) {
          stutter = true;
          last_was_stutter_ = true;   // <-- add this
          ++stutter_count_;
          total_stutter_ms_ += frame_ms;
          if (frame_ms > worst_ms_) worst_ms_ = frame_ms;
      } else {
          last_was_stutter_ = false;  // <-- add this
      }
    }

    history_.push_back(frame_ms);
    if (history_.size() > cfg_.window)
      history_.pop_front();

    ++frame_count_;
    return stutter;
  }

  // ---- Accessors used by reporting ----------------------------------------
  uint64_t frame_count()      const { return frame_count_;      }
  uint64_t stutter_count()    const { return stutter_count_;    }
  double   worst_ms()         const { return worst_ms_;         }
  double   total_stutter_ms() const { return total_stutter_ms_; }

  // Stutters per minute over the full run.
  double stutters_per_minute(double elapsed_seconds) const {
    if (elapsed_seconds <= 0.0) return 0.0;
    return stutter_count_ * 60.0 / elapsed_seconds;
  }

  // Dump a one-line summary to stderr (visible in Wine's console output).
  void dump_summary(double elapsed_seconds) const {
    std::fprintf(stderr,
      "[dxmt stutter] frames=%llu stutters=%llu (%.1f/min) "
      "worst=%.1f ms total_stall=%.0f ms\n",
      (unsigned long long)frame_count_,
      (unsigned long long)stutter_count_,
      stutters_per_minute(elapsed_seconds),
      worst_ms_,
      total_stutter_ms_);
  }

  std::string hud_line() const {
    return std::format(
        "Stutter {:3d}  Worst {:5.1f}ms{}",
        (int)stutter_count_,
        worst_ms_,
        last_was_stutter_ ? "  !!!" : ""
    );
  }

  bool last_was_stutter_ = false;

private:
  double rolling_median() const {
    // Copy into a temp vector so we don't disturb insertion order.
    std::vector<double> tmp(history_.begin(), history_.end());
    auto mid = tmp.begin() + (tmp.size() / 2);
    std::nth_element(tmp.begin(), mid, tmp.end());
    return *mid;
    // nth_element is O(n) average — fine for window=120.
  }


  Config             cfg_;
  std::deque<double> history_;
  uint64_t           frame_count_      = 0;
  uint64_t           stutter_count_    = 0;
  double             worst_ms_         = 0.0;
  double             total_stutter_ms_ = 0.0;
};

struct CompileStallStats {
    uint64_t num_stalls;
    uint64_t total_time_stalled;
};

// One global instance, defined in the header as inline so it's fine in
// multiple translation units (C++17).
inline CompileStallStats g_compile_stall_stats;

} // namespace dxmt