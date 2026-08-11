#include "services/util/log.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace svc::log {

  namespace {
    std::atomic<int> g_level{static_cast<int>(default_level)};
    std::mutex g_mutex;
  } // namespace

  void set_level(level lv) {
    g_level.store(static_cast<int>(lv), std::memory_order_relaxed);
  }

  level get_level() noexcept {
    return static_cast<level>(g_level.load(std::memory_order_relaxed));
  }

  void message(level lv, const std::string &topic, const std::string &text) {
    if (static_cast<int>(lv) < static_cast<int>(get_level()))
      return;

    auto const now = std::chrono::system_clock::now();
    auto const t = std::chrono::system_clock::to_time_t(now);
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch())
                        .count() %
                    1000;

    std::tm tm{};
    localtime_r(&t, &tm);

    std::ostringstream line;
    line << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3)
         << std::setfill('0') << ms;
    switch (lv) {
    case level::debug:
      line << " DBG";
      break;
    case level::info:
      line << " INF";
      break;
    case level::warning:
      line << " WAR";
      break;
    case level::fatal:
      line << " FAT";
      break;
    default:
      line << "   ";
    }
    if (!topic.empty())
      line << " [" << topic << ']';
    line << ' ' << text << '\n';

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fwrite(line.str().data(), 1, line.str().size(), stderr);
    std::fflush(stderr);
  }

} // namespace svc::log