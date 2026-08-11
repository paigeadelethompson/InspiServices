// AnswerServices - single threaded event loop built on poll(2).
//
// The services and bridge processes are single threaded; all sockets registered
// here are non-blocking. Timers are monotonic based. call_later() defers work
// until the next loop iteration which keeps callback reentrancy safe.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <poll.h>
#include <unordered_map>
#include <vector>

namespace svc::net {

  using Clock = std::chrono::steady_clock;

  class Reactor {
  public:
    using Callback = std::function<void()>;
    using Handle = std::uint64_t;
    static constexpr Handle bad_handle = 0;

    Reactor() = default;
    Reactor(const Reactor &) = delete;
    Reactor &operator=(const Reactor &) = delete;

    // ---- sockets ------------------------------------------------------
    // Registers a raw non-blocking fd. Returns a handle which must be used for
    // all further operations. Ownership of the fd is NOT transferred; the
    // caller closes it once remove_socket() has been called.
    Handle add_socket(int fd, Callback on_read = {}, Callback on_write = {},
                      Callback on_hangup = {});
    void set_interest(Handle h, bool read, bool write);
    void remove_socket(Handle h);

    // ---- timers -------------------------------------------------------
    Handle add_timer(std::chrono::milliseconds when, Callback fn,
                     bool repeat = false);
    void remove_timer(Handle h);

    // Defers a callback to the next loop iteration (never runs reentrantly).
    void call_later(Callback fn);

    // Submits a one-shot timeout task, similar to a timer but one-shot.
    Handle add_timeout(std::chrono::milliseconds when, Callback fn);

    // ---- main loop ----------------------------------------------------
    void run();
    void stop();
    [[nodiscard]] bool is_running() const noexcept { return running; }
    [[nodiscard]] bool is_stopped() const noexcept { return !running; }

  private:
    struct Timer {
      Clock::time_point interval_start;
      std::chrono::milliseconds period{};
      Callback fn;
      bool repeat = false;
      bool active = true;
    };

    struct SocketReg {
      int fd = -1;
      bool read = false;
      bool write = false;
      Handle generation = 0;
      Callback on_read;
      Callback on_write;
      Callback on_hang;
      bool active = false;
    };

    void prepare_sockets();
    void prepare_timers();
    Handle next_handle();

    std::vector<SocketReg> sockets_;        // indexed by handle - 1
    std::unordered_map<int, Handle> by_fd_; // fd -> handle
    std::vector<pollfd> pollfds_;
    std::deque<std::pair<Clock::time_point, Callback>> deadlines_;
    std::vector<std::pair<Handle, Timer>> timers_;
    std::uint64_t counter_ = 1;
    bool running = false;
    bool stop_ = false;
  };

} // namespace svc::net