#include "services/irc/routing.h"

#include <algorithm>

#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::irc {

  hub::hub(net::Reactor &reactor, hub_config cfg)
      : reactor_(reactor), cfg_(std::move(cfg)) {}

  hub::~hub() {
    for (auto *l : links_)
      delete l;
  }

  void hub::manage(link &l) {
    l.on_line = [this, &l](link &, message const &msg) { route_from(l, msg); };
    l.on_burst = [this, &l](link &) { send_our_burst(l); };
    l.on_close = [this](link &dead) {
      auto it = std::find(links_.begin(), links_.end(), &dead);
      if (it != links_.end())
        links_.erase(it);
      if (on_link_down)
        on_link_down(dead);
    };
    l.on_reconnect = [this](link &alive) {
      // A reconnecting uplink re-registers itself once its transport is back.
      if (std::find(links_.begin(), links_.end(), &alive) == links_.end())
        links_.push_back(&alive);
    };
    if (std::find(links_.begin(), links_.end(), &l) == links_.end())
      links_.push_back(&l);
  }

  link &hub::add_uplink(link_config cfg) {
    cfg.server_name = cfg_.server_name;
    cfg.server_sid = cfg_.server_sid;
    cfg.server_desc = cfg_.server_desc;
    auto *l = new link(reactor_, cfg);
    link &ref = *l;
    manage(ref);
    ref.connect();
    return ref;
  }

  link &hub::add_inbound(int fd, std::shared_ptr<net::tls_session> tls) {
    link_config cfg;
    cfg.server_name = cfg_.server_name;
    cfg.server_sid = cfg_.server_sid;
    cfg.server_desc = cfg_.server_desc;
    cfg.accept = true;
    auto *l = new link(reactor_, cfg);
    link &ref = *l;
    manage(ref);
    ref.adopt(fd, std::move(tls));
    return ref;
  }

  void hub::route_from(link &from, message const &msg) {
    // Decide whether this message is meant for us only, or must be forwarded
    // to the other links.
    if (targets_self(msg)) {
      // Private message for one of our own users: handle it locally.
      if (on_message)
        on_message(from, msg);
      return;
    }

    // Broadcast to every other linked server.
    for (link *other : links_) {
      if (other == &from || !other->linked())
        continue;
      other->send(msg);
    }

    if (on_message)
      on_message(from, msg);
  }

  bool hub::targets_self(message const &msg) const {
    std::string const &cmd = msg.command;
    if (cmd == "PRIVMSG" || cmd == "NOTICE") {
      if (msg.params.size() < 2)
        return true;
      std::string const &target = msg.params[0];
      if (target.rfind(cfg_.server_sid, 0) == 0)
        return true;
      if (target == cfg_.server_name)
        return true;
      return false;
    }
    return false;
  }

  void hub::broadcast(message const &msg) {
    for (link *other : links_) {
      if (other->linked())
        other->send(msg);
    }
  }

  void hub::send_our_burst(link &to) {
    // Announce every link (child server) that is not `to` itself, then the
    // users and channels we host.
    for (link *other : links_) {
      if (other == &to || other->remote_sid().empty())
        continue;
      to.send_line(":" + cfg_.server_sid + " SERVER " + other->remote_name() +
                   " " + other->remote_sid() + " :" + cfg_.server_desc);
    }
    if (on_burst)
      on_burst(to);
  }

} // namespace svc::irc