#include "services/irc/network.h"

#include <algorithm>
#include <cctype>

namespace svc::irc {
  namespace {

    std::string rfc1459_fold(std::string_view s) {
      std::string out(s);
      for (char &c : out) {
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c + ('a' - 'A'));
        else if (c == '[')
          c = '{';
        else if (c == ']')
          c = '}';
        else if (c == '\\')
          c = '|';
        else if (c == '~')
          c = '^';
      }
      return out;
    }

  } // namespace

  void network::add_user(user const &u) {
    auto it = users_.find(u.uid);
    if (it != users_.end() && it->second.nick != u.nick)
      nicks_.erase(rfc1459_fold(it->second.nick));
    auto [_, inserted] = users_.insert_or_assign(u.uid, u);
    if (inserted)
      nicks_[rfc1459_fold(u.nick)] = u.uid;
  }

  void network::remove_user(std::string_view uid) {
    std::string key(uid);
    auto it = users_.find(key);
    if (it == users_.end())
      return;
    nicks_.erase(rfc1459_fold(it->second.nick));
    users_.erase(it);
  }

  void network::rename_user(std::string_view uid, std::string newnick) {
    std::string key(uid);
    auto it = users_.find(key);
    if (it == users_.end())
      return;
    nicks_.erase(rfc1459_fold(it->second.nick));
    it->second.nick = std::move(newnick);
    nicks_[rfc1459_fold(it->second.nick)] = std::string(uid);
  }

  user *network::find_user(std::string_view uid) {
    auto it = users_.find(std::string(uid));
    if (it == users_.end())
      return nullptr;
    return &it->second;
  }

  user *network::by_nick(std::string_view nick) {
    auto it = nicks_.find(rfc1459_fold(nick));
    if (it == nicks_.end())
      return nullptr;
    return find_user(it->second);
  }

  std::vector<user *> network::all_users() {
    std::vector<user *> out;
    out.reserve(users_.size());
    for (auto &[uid, u] : users_)
      (void)uid, out.push_back(&u);
    return out;
  }

  channel *network::find_channel(std::string_view name) {
    auto it = channels_.find(std::string(name));
    if (it == channels_.end())
      return nullptr;
    return &it->second;
  }

  channel &network::get_channel(std::string name) { return channels_[name]; }

  bool network::channel_exists(std::string_view name) const {
    return channels_.count(std::string(name)) != 0;
  }

  std::vector<channel *> network::all_channels() {
    std::vector<channel *> out;
    out.reserve(channels_.size());
    for (auto &[name, c] : channels_)
      (void)name, out.push_back(&c);
    return out;
  }

} // namespace svc::irc