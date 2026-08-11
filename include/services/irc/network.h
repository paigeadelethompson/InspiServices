// AnswerServices — in-memory IRC network model.
//
// The services core keeps a soft view of the network state so it can resolve
// nicknames to accounts, look up channel members, and feed the services.
// This model is not a full ircd: it only tracks a small subset of the data
// the services actually consume.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svc::irc {

  struct user {
    std::string uid; // "<sid>xxxx" server-prefixed unique id
    std::string nick;
    std::string realhost;    // real hostname
    std::string displayhost; // hostname as shown (cloaked/or real)
    std::string realuser;    // ident
    std::string displayuser; // ident as shown
    std::string ip;          // networking ip if known
    std::string realname;
    std::string account{}; // logged-in account or empty
    std::string certfp{}; // client TLS cert fingerprint, if any
    std::string awaymsg{};
    std::string mode; // user modes e.g. "+iw"
    std::string opertype{}; // ircd oper class name (OPERTYPE), "" if not oper
    std::int64_t nickchanged{};
    std::int64_t signon{};
    std::int64_t ts{};  // uid ctime / nickchange time
    std::string server; // server name this user is on (empty = us)

    [[nodiscard]] std::string fullmask() const {
      return nick + "!" + displayuser + "@" + displayhost;
    }
  };

  // A channel as far as the services know it. `members` only contains users on
  // channels the services can see (they are fed by the link when it receives
  // events). Members map uid -> status letters such as "ov".
  struct channel {
    std::string name;
    std::int64_t modelock{};
    std::string topic;
    std::string topicsetby;
    std::int64_t topicset{};
    std::string modes;                          // e.g. "+ntk"
    std::vector<std::string> params;            // mode key/limit etc.
    std::map<std::string, std::string> members; // uid -> statuses "ov"
  };

  class network {
  public:
    // -- users --------------------------------------------------------------
    void add_user(user const &u); // add or replace by uid
    void remove_user(std::string_view uid);
    // Renames a user (already added) and fixes the nick->uid index.
    void rename_user(std::string_view uid, std::string newnick);
    user *find_user(std::string_view uid);
    user *by_nick(std::string_view nick); // rfc1459 folded
    std::vector<user *> all_users();

    // -- channels -----------------------------------------------------------
    channel *find_channel(std::string_view name);
    channel &get_channel(std::string name); // create-if-missing
    bool channel_exists(std::string_view name) const;
    std::vector<channel *> all_channels();

    // -- server table immaterial to services --------------------------------
    void set_server_name(std::string_view name) { server_name_ = name; }
    std::string const &server_name() const { return server_name_; }

  private:
    std::map<std::string, user> users_;        // uid -> user
    std::map<std::string, std::string> nicks_; // folded nick -> uid
    std::map<std::string, channel> channels_;  // name -> channel
    std::string server_name_;
  };

} // namespace svc::irc