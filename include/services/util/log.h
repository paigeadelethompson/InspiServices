// InspiServices - small logging facility.
//
// Log level and verbose flags live in the database (config table), because the
// project deliberately ships without configuration files. Startup order still
// needs a destination, so an early "raw mode" is provided that merely emits to
// stderr until the DB is available.
#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <string>

namespace svc::log {

  enum class level : int {
    debug = 0,
    info = 1,
    warning = 2,
    fatal = 3,
    nothing = 4,
  };

  // Compiled-in default used before configuration is loaded.
  inline constexpr level default_level = level::info;

  // Sets the minimum level printed on this stream.
  void set_level(level lv);
  level get_level() noexcept;

  void message(level lv, const std::string &topic, const std::string &text);

  template <typename... Args>
  void msg(level lv, const std::string &topic, const std::string &fmt,
           Args &&...args) {
    if (static_cast<int>(lv) < static_cast<int>(get_level()))
      return;
    message(lv, topic, std::vformat(fmt, std::make_format_args(args...)));
  }

  template <typename... Args>
  inline void debug(const std::string &topic, const std::string &f,
                    Args &&...args) {
    ::svc::log::msg(::svc::log::level::debug, topic, f,
                    std::forward<Args>(args)...);
  }

  template <typename... Args>
  inline void info(const std::string &topic, const std::string &f,
                   Args &&...args) {
    ::svc::log::msg(::svc::log::level::info, topic, f,
                    std::forward<Args>(args)...);
  }

  template <typename... Args>
  inline void warn(const std::string &topic, const std::string &f,
                   Args &&...args) {
    ::svc::log::msg(::svc::log::level::warning, topic, f,
                    std::forward<Args>(args)...);
  }

  template <typename... Args>
  inline void error(const std::string &topic, const std::string &f,
                    Args &&...args) {
    ::svc::log::msg(::svc::log::level::warning, topic, f,
                    std::forward<Args>(args)...);
  }

} // namespace svc::log