// AnswerServices - utilities shared by the service modules.
#pragma once

#include <string>
#include <string_view>

#include "services/services/core.h"
#include "services/util/util.h"

namespace svc::core {

  // Folds a nick/ident/account name for DB storage (RFC1459 folding).
  inline std::string fold(std::string_view s) { return sv::irc_lower(s); }

  // True if the given user (uid) has an oper user mode (or was announced as an
  // ircd operator via OPERTYPE).
  inline bool is_oper(ctx &c, std::string_view uid) {
    irc::user *u = c.net().find_user(uid);
    return u != nullptr &&
           (u->mode.find_first_of("Oo") != std::string::npos ||
            !u->opertype.empty());
  }

  // ---- shared channel access ----------------------------------------------
  // Founder account (folded) of a channel, "" if unregistered.
  inline std::string channel_founder(ctx &c, std::string_view chan) {
    auto rows = c.database().query(
        "SELECT founder FROM chanserv WHERE name=? LIMIT 1",
        {std::string(sv::irc_lower(chan))});
    return rows.empty() ? std::string() : rows[0].as_string("founder");
  }

  // Access level of `who` (nick or account, never founder-level) on a channel.
  inline int channel_access_level(ctx &c, std::string_view chan,
                                  std::string_view who) {
    auto rows = c.database().query(
        "SELECT level FROM chanserv_access WHERE channel=? AND who=? LIMIT 1",
        {std::string(sv::irc_lower(chan)), fold(who)});
    if (rows.empty())
      return -100;
    return static_cast<int>(rows[0].as_int("level", -100));
  }

  // True if `u` has at least `min` access on `chan`. Founders always pass.
  inline bool can_chan(ctx &c, irc::user const &u, std::string_view chan,
                       int min) {
    std::string const founder = channel_founder(c, chan);
    if (!founder.empty() && !u.account.empty() &&
        founder == fold(u.account))
      return true;
    if (channel_access_level(c, chan, u.nick) >= min)
      return true;
    if (!u.account.empty() &&
        channel_access_level(c, chan, u.account) >= min)
      return true;
    return false;
  }

  // Sends a channel mode change from a service actor (e.g. ChanServ) and
  // updates nothing locally - the ircd owns the channel mode state.
  inline void send_chan_mode(ctx &c, std::string_view source,
                             std::string_view chan, std::string_view modes,
                             std::string_view param = {}) {
    irc::channel *ch = c.net().find_channel(chan);
    std::int64_t const ts =
        ch && ch->modelock > 0 ? ch->modelock : svc::irc::now();
    irc::message m;
    m.prefix = std::string(source);
    m.command = "FMODE";
    m.params.push_back(std::string(chan));
    m.params.push_back(std::to_string(ts));
    m.params.push_back(std::string(modes));
    if (!param.empty())
      m.params.push_back(std::string(param));
    c.deliver(m);
  }

  // Makes a channel's registration live (shared by ChanServ's REGISTER auto
  // path and OperServ's pending approval): creates/replaces the chanserv row,
  // gives the founder OWNER (600) access and flags the live channel +r.
  inline void activate_channel(ctx &c, std::string_view chan,
                               std::string_view founder) {
    std::string const key(sv::irc_lower(chan));
    std::int64_t const now = svc::irc::now();
    c.database().run(
        "INSERT OR REPLACE INTO chanserv (name, founder, password, salt, "
        "modes, topic, registered, lastused) VALUES (?, ?, '', '', '+rnt', "
        "'', ?, ?)",
        {key, fold(founder), now, now});
    c.database().run(
        "INSERT OR REPLACE INTO chanserv_access (channel, who, level) "
        "VALUES (?, ?, 600)",
        {key, fold(founder)});
    if (c.net().find_channel(chan) != nullptr)
      send_chan_mode(c, c.service_uid("ChanServ"), chan, "+r");
  }

} // namespace svc::core