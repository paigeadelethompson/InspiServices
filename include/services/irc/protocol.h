// AnswerServices - InspIRCd spanning tree wire protocol.
//
// Implements the message format used by the current InspIRCd protocol
// (PROTO_NEWEST = 1206): @tags <prefix> <COMMAND> <params...> :trailing.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace svc::irc {

  // Returns true for characters that may appear at the start of a FJOIN member
  // specification.
  bool is_mode_char(char c);

  // Current unix time in seconds.
  [[nodiscard]] std::int64_t now();

  // True if every character of the given string is a valid UID/nick character.
  bool is_legal_sid(std::string_view s);
  bool is_legal_uid(std::string_view s);
  bool is_legal_nick(std::string_view s);

  inline constexpr std::uint16_t protocol_version = 1206;

  struct messages {
    static constexpr std::string_view server = "SERVER";
    static constexpr std::string_view capab = "CAPAB";
    static constexpr std::string_view burst = "BURST";
    static constexpr std::string_view end_burst = "ENDBURST";
    static constexpr std::string_view uid = "UID";
    static constexpr std::string_view fmode = "FMODE";
    static constexpr std::string_view fjoin = "FJOIN";
    static constexpr std::string_view ftopic = "FTOPIC";
    static constexpr std::string_view oper = "OPERTYPE";
    static constexpr std::string_view away = "AWAY";
    static constexpr std::string_view metadata = "METADATA";
    static constexpr std::string_view privmsg = "PRIVMSG";
    static constexpr std::string_view notice = "NOTICE";
    static constexpr std::string_view nick = "NICK";
    static constexpr std::string_view quit = "QUIT";
    static constexpr std::string_view sjoin = "SVSJOIN";
    static constexpr std::string_view spart = "SVSPART";
    static constexpr std::string_view snick = "SVSNICK";
    static constexpr std::string_view shold = "SVSHOLD";
    static constexpr std::string_view soper = "SVSOPER";
    static constexpr std::string_view scmode = "SVSCMODE";
    static constexpr std::string_view stopic = "SVSTOPIC";
    static constexpr std::string_view kill = "KILL";
    static constexpr std::string_view squit = "SQUIT";
    static constexpr std::string_view notify = "NOTICE";
    static constexpr std::string_view ping = "PING";
    static constexpr std::string_view pong = "PONG";
    static constexpr std::string_view globops = "GLOBOPS";
    static constexpr std::string_view encap = "ENCAP";
    static constexpr std::string_view mode = "MODE";
    static constexpr std::string_view topic = "TOPIC";
    static constexpr std::string_view aline = "ADDLINE";
    static constexpr std::string_view dline = "DELLINE";
  };

  struct message {
    std::vector<std::pair<std::string, std::string>>
        tags;           // name, value ("" for bare)
    std::string prefix; // sid or uid, empty = our peer
    std::string command;
    std::vector<std::string> params;

    // Parses a single wire line (without leading/trailing newline).
    static message parse(std::string_view line);

    // Serialises; if `trailing_colon` is true the last parameter is always
    // prefixed with ':' which is required for trailing text parameters.
    std::string to_wire() const;

    [[nodiscard]] std::optional<std::string> tag(std::string_view key) const;
    [[nodiscard]] std::string param_or(std::size_t idx,
                                       std::string_view def = {}) const;
    [[nodiscard]] bool empty() const noexcept { return command.empty(); }
  };

  // Building helpers ----------------------------------------------------------
  // Produces "PREFIX COMMAND p1 p2 :last" strings used when talking to a hub.
  std::string build_line(std::string_view prefix, std::string_view command,
                         std::span<const std::string> params);

  std::string
  prefix_last(std::string_view text); // ":text" (adds ':' if needed)

  // Splits channel membership list for FJOIN last parameter -> vector of
  // "prefixmask<uuid>".
  std::vector<std::string> parse_member_list(std::string_view joined);

} // namespace svc::irc