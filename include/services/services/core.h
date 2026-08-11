// AnswerServices - central daemon context.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "services/config.h"
#include "services/db.h"
#include "services/irc/link.h"
#include "services/irc/network.h"
#include "services/irc/protocol.h"
#include "services/irc/routing.h"
#include "services/net/eventloop.h"

namespace svc::bridge {
  class manager;
}

namespace svc::core {

  // A service is a pseudo-user presented to the network with a fixed nick/uid.
  struct service_info {
    std::string name;  // e.g. "NickServ"
    std::string ident; // e.g. "nickserv"
    std::string uid;   // unique, e.g. "8E000001"
    std::string gecos;
    std::string host = "services.answer"; // client host shared by all vhosts
    bool oper = false;
  };

  // A single user->service private message.
  struct cmsg {
    service_info const *service = nullptr;
    std::string sender;            // sender UID
    std::string nick;              // sender nick
    std::string sender_account;    // "" if not logged in
    std::string reply;             // uid to reply to
    std::string text;              // full text after target
    std::string command;           // upper-cased first word
    std::vector<std::string> args; // words after command

    [[nodiscard]] std::string arg(std::size_t i) const {
      return i < args.size() ? args[i] : std::string();
    }
    [[nodiscard]] std::size_t argc() const noexcept { return args.size(); }
    [[nodiscard]] std::string join(std::size_t from = 0) const {
      std::string out;
      for (std::size_t i = from; i < args.size(); ++i) {
        if (i != from)
          out.push_back(' ');
        out += args[i];
      }
      return out;
    }
  };

  class ctx {
  public:
    using command_fn = std::function<void(ctx &, cmsg const &)>;

    ctx(net::Reactor &reactor, db &database, config &cfg, irc::hub &hub);
    ~ctx();

    net::Reactor &the_reactor() noexcept { return reactor_; }
    db &database() noexcept { return db_; }
    config &cfg() noexcept { return cfg_; }
    irc::hub &the_hub() noexcept { return hub_; }
    irc::network &net() noexcept { return hub_.state(); }

    // Optional: bridge manager (set by main after install if bridges are used).
    void set_bridge_manager(svc::bridge::manager &bm) noexcept {
      bridge_manager_ = &bm;
    }
    svc::bridge::manager *bridge_manager() noexcept { return bridge_manager_; }

    // Registers a service pseudo-user and makes it known to every link.
    service_info &add_service(std::string name, std::string gecos,
                              bool oper = false);

    // The registered service users.
    std::vector<std::shared_ptr<service_info>> const &
    services() const noexcept {
      return services_;
    }

    // Registers a command handler for `service` (matching a service nick).
    // `cmd` is matched case-insensitively.
    void on_command(std::string_view service, std::string_view cmd,
                    command_fn fn);

    // Registers help text for `subject` (e.g. "REGISTER") on a service. The
    // first line should be "Usage: ..."; HELP lists it, HELP <subject> shows
    // the whole entry.
    void add_help(std::string_view service, std::string_view subject,
                  std::string text);

    // Implements the generic HELP command (registered for every service).
    void help_command(cmsg const &m);

    // Generic "help" text bank: service -> SUBJECT -> text.
    using help_bank = std::map<std::string, std::map<std::string, std::string>,
                               std::less<>>;

    // ---- sending ----
    void send_notice(service_info const &sv, std::string_view target,
                     std::string_view text);
    void reply(cmsg const &m, std::string_view text);
    void notice(cmsg const &m, std::string_view text);

    // Pushes a fully-formed origin message onto every linked hub.
    void deliver(irc::message const &m);

    // Fired for every inbound message.
    void on_line(irc::message const &m);

    // Registers a receiver for channel-targeted PRIVMSG/NOTICE messages (used
    // by the bridge relay and BotServ's seen tracker). Multiple receivers run
    // in registration order.
    void add_channel_message(std::function<void(irc::message const &)> fn);

    // Registers a receiver fired after a channel's membership changes. Modules
    // (BotServ) keep assigned service bots joined here. Multiple receivers run
    // in registration order.
    void add_channel_state(std::function<void(irc::channel &)> fn);

    // Registers an in-channel fantasy (leading '!') command receiver.
    void add_fantasy(std::function<void(irc::user const &, std::string_view,
                                        std::string_view)>
                         fn);

    // Fired for a user joining a channel (IJOIN). Used e.g. by ChanServ to
    // enforce AKICK entries.
    std::function<void(irc::user const &, irc::channel &)> on_user_join;

    // Fired when a user's TLS certfp is known/changed. Used by NickServ to
    // auto-identify against bound certificates.
    std::function<void(irc::user &)> on_user_cert;

    // Extra pseudo-users to include in send_burst() in addition to the normal
    // service users (runtime bots, etc).
    std::function<void(irc::link &)> on_burst_extra;

    // Install all modules.
    void install();

    // Sends the pseudo-user burst for `link`.
    void introduce_to(irc::link &link);

    // UID of a registered service by (case-insensitive) name; "" if absent.
    std::string service_uid(std::string_view name) const;

    // Allocates a new, unique user UID for modules that manage pseudo-users
    // (e.g. BotServ bots). Stays unique across service users and the network.
    std::string allocate_uid();

    // Records that `nick` (folded) last spoke at `ts` in channel chat.
    void note_seen(std::string_view nick, std::int64_t ts);
    // Last-seen timestamp for `nick` (folded), or 0 if unknown.
    [[nodiscard]] std::int64_t seen_at(std::string_view nick) const;

  private:
    void handle_uid(irc::message const &m);
    void handle_fjoin(irc::message const &m);
    void handle_ijoin(irc::message const &m);
    void handle_part(irc::message const &m);
    void handle_kick(irc::message const &m);
    void handle_topic(irc::message const &m);
    void handle_squit(irc::message const &m);
    void handle_away(irc::message const &m);
    void handle_fmode(irc::message const &m);
    void handle_opertype(irc::message const &m);
    void handle_mode(irc::message const &m);
    void handle_nick(irc::message const &m);
    void handle_quit(irc::message const &m);
    void handle_privmsg(irc::message const &m);
    void handle_account(irc::message const &m);

    net::Reactor &reactor_;
    db &db_;
    config &cfg_;
    irc::hub &hub_;

    std::vector<std::shared_ptr<service_info>> services_;
    // lower-service -> map<UPPERCMD,fn>
    std::map<std::string, std::map<std::string, command_fn>, std::less<>>
        commands_;
    help_bank help_;
    std::vector<std::function<void(irc::message const &)>> channel_messages_;
    std::vector<std::function<void(irc::channel &)>> channel_states_;
    std::vector<std::function<void(irc::user const &, std::string_view,
                                   std::string_view)>>
        fantasies_;
    // folded nick -> last channel-spoke unix time (in-memory seen cache).
    std::unordered_map<std::string, std::int64_t> seen_;
    unsigned uid_counter_ = 1;
    svc::bridge::manager *bridge_manager_ = nullptr;
  };

  // Module installs (defined in the various services/*.cpp).
  void install_nickserv(ctx &c);
  void install_chanserv(ctx &c);
  void install_botserv(ctx &c);
  void install_operserv(ctx &c);
  void install_bridgeserv(ctx &c);

} // namespace svc::core