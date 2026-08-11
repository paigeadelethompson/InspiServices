#include "services/net/eventloop.h"

#include <poll.h>

#include <algorithm>

namespace svc::net {

  Reactor::Handle Reactor::next_handle() {
    // Handles are monotonically increasing; vectors grow and slots are never
    // reused, so a handle identifies a unique slot for the process lifetime.
    return static_cast<Handle>(sockets_.size() + 1);
  }

  Reactor::Handle Reactor::add_socket(int fd, Callback on_read,
                                      Callback on_write, Callback on_hang) {
    if (fd < 0)
      return bad_handle;
    Handle const h = next_handle();
    SocketReg reg{};
    reg.fd = fd;
    reg.read = bool(on_read);
    reg.write = bool(on_write);
    reg.generation = h;
    reg.on_read = std::move(on_read);
    reg.on_write = std::move(on_write);
    reg.on_hang = std::move(on_hang);
    reg.active = true;
    sockets_.push_back(std::move(reg));
    by_fd_.emplace(fd, h);
    return h;
  }

  void Reactor::set_interest(Handle h, bool read, bool write) {
    auto const idx = static_cast<std::size_t>(h - 1);
    if (idx >= sockets_.size())
      return;
    SocketReg &reg = sockets_[idx];
    reg.read = read;
    reg.write = write;
  }

  void Reactor::remove_socket(Handle h) {
    auto const idx = static_cast<std::size_t>(h - 1);
    if (idx >= sockets_.size())
      return;
    SocketReg &reg = sockets_[idx];
    if (!reg.active)
      return;
    reg.active = false;
    by_fd_.erase(reg.fd);
  }

  Reactor::Handle Reactor::add_timer(std::chrono::milliseconds when,
                                     Callback fn, bool repeat) {
    Timer t;
    t.period = when;
    t.fn = std::move(fn);
    t.repeat = repeat;
    t.active = true;
    t.interval_start = Clock::now();
    Handle const h = timers_.size() + 1;
    timers_.emplace_back(h, std::move(t));
    return h;
  }

  Reactor::Handle Reactor::add_timeout(std::chrono::milliseconds when,
                                       Callback fn) {
    return add_timer(when, std::move(fn), false);
  }

  void Reactor::remove_timer(Handle h) {
    for (auto &[id, t] : timers_)
      if (id == h) {
        t.active = false;
        return;
      }
  }

  void Reactor::call_later(Callback fn) {
    deadlines_.emplace_back(Clock::now(), std::move(fn));
  }

  void Reactor::stop() { stop_ = true; }

  void Reactor::run() {
    if (running)
      return;
    running = true;
    stop_ = false;

    while (!stop_) {
      // Build poll descriptors once per iteration.
      pollfds_.clear();
      for (auto &reg : sockets_) {
        if (!reg.active)
          continue;
        pollfd p{};
        p.fd = reg.fd;
        p.events = 0;
        if (reg.read)
          p.events |= POLLIN;
        if (reg.write)
          p.events |= POLLOUT;
        pollfds_.push_back(p);
      }

      auto const now = Clock::now();

      // Compute the nearest timeout (minimum of all timers/deadlines).
      std::chrono::milliseconds timeout = pollfds_.empty()
                                              ? std::chrono::milliseconds(500)
                                              : std::chrono::milliseconds(1000);
      auto consider_timeout = [&](Clock::time_point when) {
        auto const remain = when - now;
        auto const ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(remain);
        if (ms > std::chrono::milliseconds(0) && ms < timeout)
          timeout = ms;
        else if (ms <= std::chrono::milliseconds(0))
          timeout = std::chrono::milliseconds(0);
      };
      for (auto const &[_, t] : timers_)
        if (t.active)
          consider_timeout(t.interval_start + t.period);
      for (auto const &[at, _fn] : deadlines_)
        consider_timeout(at);
      if (timeout < std::chrono::milliseconds(0))
        timeout = std::chrono::milliseconds(0);

      int const rc =
          ::poll(pollfds_.data(), static_cast<nfds_t>(pollfds_.size()),
                 static_cast<int>(timeout.count()));
      (void)rc;

      auto const now2 = Clock::now();

      // Dispatch socket events.
      for (std::size_t i = 0; i < pollfds_.size(); ++i) {
        short const revents = pollfds_[i].revents;
        if (revents == 0)
          continue;
        auto const it = by_fd_.find(pollfds_[i].fd);
        if (it == by_fd_.end())
          continue;
        auto const idx = static_cast<std::size_t>(it->second - 1);
        if (idx >= sockets_.size())
          continue;
        SocketReg &reg = sockets_[idx];
        if (!reg.active)
          continue;
        if (revents & POLLNVAL) {
          if (reg.on_hang)
            reg.on_hang();
          continue;
        }
        if ((revents & POLLIN) && reg.on_read)
          reg.on_read();
        if ((revents & POLLOUT) && reg.on_write)
          reg.on_write();
        if ((revents & (POLLHUP | POLLERR)) && reg.on_hang)
          reg.on_hang();
      }

      // Fire deadlines (deferred callbacks).
      std::deque<std::pair<Clock::time_point, Callback>> due;
      while (!deadlines_.empty() && deadlines_.front().first <= now2) {
        due.push_back(std::move(deadlines_.front()));
        deadlines_.pop_front();
      }
      for (auto &[at, fn] : due)
        if (fn)
          fn();

      // Fire timers. Index-based so callbacks that schedule new timers
      // (push_back) do not invalidate the iteration.
      for (std::size_t i = 0; i < timers_.size(); ++i) {
        auto &[h, t] = timers_[i];
        if (!t.active)
          continue;
        if (t.interval_start + t.period <= now2) {
          if (t.repeat)
            t.interval_start = now2;
          else
            t.active = false;
          t.fn();
        }
      }
    }

    running = false;
  }

} // namespace svc::net