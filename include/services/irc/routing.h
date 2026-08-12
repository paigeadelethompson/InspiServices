// InspiServices - hub / router.
//
// Owns every active link (a server connection), maintains the network model,
// and decides which messages to forward to which links. The services core and
// bridge core drive this router to speak to InspIRCd.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "services/irc/link.h"
#include "services/irc/network.h"

namespace svc::irc {

  struct hub_config {
    std::string server_name; // our server name in the network
    std::string server_sid;  // our 3-char SID
    std::string server_desc;
  };

  // The hub implements the wiring that lets services operate and bridges relay.
  class hub {
  public:
    using line_handler = std::function<void(link &, message const &)>;

    hub(net::Reactor &reactor, hub_config cfg);
    ~hub();

    // ---- link management ---------------------------------------------------
    // Creates an outbound link to an InspIRCd hub and starts connecting.
    link &add_uplink(link_config cfg);
    // Registers an accepted inbound socket (bridge service connection).
    link &add_inbound(int fd, std::shared_ptr<net::tls_session> tls);
    // Registers a link that already connected outbound (kept for ownership).
    void manage(link &l);

    std::vector<link *> const &links() const noexcept { return links_; }

    [[nodiscard]] net::Reactor &reactor() noexcept { return reactor_; }

    // ---- config -------------------------------------------------------------
    [[nodiscard]] hub_config const &cfg() const noexcept { return cfg_; }

    // ---- network state -----------------------------------------------------
    network &state() noexcept { return net_; }

    // ---- routing --------------------------------------------------------
    // Forwards a message received on `from` to every other connected link,
    // applying InspIRCd routing rules (never route a message back to the link
    // it came from, and never forward a message headed to our own users).
    void route_from(link &from, message const &msg);

    // Sends a message sourced from our server to every link suitable.
    void broadcast(message const &msg);

    // ---- event hooks ----------------------------------------------------
    std::function<void(link &)> on_link_up; // link became operational
    std::function<void(link &)>
        on_link_down;                     // link dropped (link already dead)
    std::function<void(link &)> on_burst; // fill our burst on a new link
    line_handler on_message;              // received any message

  private:
    bool targets_self(message const &msg) const;
    void send_our_burst(link &to);

    net::Reactor &reactor_;
    hub_config cfg_;
    network net_;
    std::vector<link *> links_;
  };

} // namespace svc::irc