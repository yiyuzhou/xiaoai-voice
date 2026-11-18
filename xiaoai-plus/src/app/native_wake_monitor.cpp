#include "app/native_wake_monitor.hpp"

#include <fstream>
#include <string_view>
#include <sys/stat.h>

namespace xiaoai_plus::app {
namespace {
std::string_view Lx06WakeAction(std::string_view line) {
  constexpr std::string_view kTag = "wakeup.sh: ";
  const auto tag = line.find(kTag);
  if (tag == std::string_view::npos) return {};
  line.remove_prefix(tag + kTag.size());
  return line.substr(0, line.find_first_of(" \t\r"));
}
}  // namespace

NativeWakeMonitor::NativeWakeMonitor(std::function<void()> on_wake, std::string path,
                                     bool lx06)
    : on_wake_(std::move(on_wake)), path_(std::move(path)),
      lx06_(lx06) {}

NativeWakeMonitor::~NativeWakeMonitor() { Stop(); }

void NativeWakeMonitor::Start() {
  stop_.store(false);
  thread_ = std::thread([this] { Loop(); });
}

void NativeWakeMonitor::Stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

bool NativeWakeMonitor::Blocked() const {
  std::lock_guard<std::mutex> lock(mu_);
  return (!lx06_ && native_active_) || Clock::now() < hold_until_;
}

void NativeWakeMonitor::ObserveLine(const std::string& line) {
  // Only the real wake event from the native PNS process opens the interlock.
  const bool pns_wake = line.find("mipns-xiaomi[") != std::string::npos &&
                        line.find("send real_wakeup event") != std::string::npos;
  const bool native_asr_start = line.find("wakeup.sh: asrstart") != std::string::npos;
  const auto action = lx06_ ? Lx06WakeAction(line) : std::string_view{};
  const bool lx06_wake = action == "WuW" || action == "WuW_oneshot" ||
                         action == "WuW_first" || action == "WuW_first_unregister";
  const bool lx06_end = action == "ready" || action == "ready_delay" ||
                        action == "stop" || action == "continous_md_end";
  if (pns_wake || native_asr_start || lx06_wake) {
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      first = !native_active_ || Clock::now() >= hold_until_;
      native_active_ = true;
      hold_until_ = lx06_ ? Clock::now() + std::chrono::seconds(15)
                          : Clock::time_point::max();
    }
    if (first && on_wake_) on_wake_();
  } else if (lx06_end || line.find("wakeup.sh: asrend") != std::string::npos) {
    std::lock_guard<std::mutex> lock(mu_);
    if (native_active_) {
      native_active_ = false;
      hold_until_ = Clock::now() + std::chrono::seconds(5);
    }
  }
}

void NativeWakeMonitor::Loop() {
  ino_t inode = 0;
  std::streamoff offset = 0;
  std::string partial;
  while (!stop_.load()) {
    struct stat st {};
    if (stat(path_.c_str(), &st) == 0) {
      if (inode == 0) {
        inode = st.st_ino;
        offset = st.st_size;  // Ignore old or rotated history.
        partial.clear();
      } else if (inode != st.st_ino || st.st_size < offset) {
        inode = st.st_ino;
        offset = 0;  // Inspect new lines written during rotation.
        partial.clear();
      } else if (st.st_size > offset) {
        std::ifstream file(path_, std::ios::binary);
        if (file) {
          file.seekg(offset);
          char buf[4096];
          while (file && !stop_.load()) {
            file.read(buf, sizeof(buf));
            const auto n = file.gcount();
            if (n <= 0) break;
            offset += n;
            partial.append(buf, static_cast<size_t>(n));
            size_t end;
            while ((end = partial.find('\n')) != std::string::npos) {
              ObserveLine(partial.substr(0, end));
              partial.erase(0, end + 1);
            }
            if (partial.size() > 16384) partial.clear();
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace xiaoai_plus::app
