// InspiServices - the one daemon: InspIRCd services + bridge links.
#include <csignal>
#include <cstdlib>
#include <string>

#include "services/bridge/bridge.h"
#include "services/config.h"
#include "services/db.h"
#include "services/irc/link.h"
#include "services/irc/routing.h"
#include "services/net/eventloop.h"
#include "services/services/core.h"
#include "services/services/schema.h"
#include "services/util/env.h"
#include "services/util/log.h"
#include "services/util/util.h"

namespace {

  svc::net::Reactor *g_reactor = nullptr;

  void on_signal(int sig) {
    if (g_reactor) {
      svc::log::warn("main", "caught signal {}, shutting down", sig);
      g_reactor->stop();
    }
  }

  std::string db_path = "inspiservices.db";
  std::string env_file;

  void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "-h" || arg == "--help") {
        std::printf("Usage: inspiservices [--db path] [--env path]\n");
        std::exit(0);
      }
      if ((arg == "--db" || arg == "-d") && i + 1 < argc)
        db_path = argv[++i];
      else if (arg == "--env" && i + 1 < argc)
        env_file = argv[++i];
    }
  }

} // namespace

int main(int argc, char **argv) {
  parse_args(argc, argv);
  svc::env::load_env_file(env_file);

  svc::log::set_level(svc::env::get_bool("SERVICES_DEBUG", false)
                          ? svc::log::level::debug
                          : svc::log::default_level);

  svc::db database(db_path);
  svc::db_schema(database);
  svc::config cfg(database);
  svc::net::Reactor reactor;
  g_reactor = &reactor;

  // Network identity and the uplink all come from the environment/.env file.
  // Nothing here is seeded from a config file: `SERVER_*` names this daemon
  // on the spanning tree and `HUB_*` (plus LINK_SENDPASS/LINK_RECVPASS) drive
  // the outbound link to the network core hub.
  svc::irc::hub_config hcfg;
  hcfg.server_name =
      svc::env::get("SERVER_NAME").value_or("services.netcrave.chat");
  hcfg.server_sid = svc::env::get("SERVER_SID").value_or("8E0");
  if (!svc::irc::is_legal_sid(hcfg.server_sid)) {
    svc::log::error("main", "SERVER_SID '{}' is not a 3-digit SID",
                    hcfg.server_sid);
    return 1;
  }
  hcfg.server_desc = svc::env::get("SERVER_DESC").value_or("InspiServices");

  svc::irc::hub hub(reactor, hcfg);
  svc::core::ctx core(reactor, database, cfg, hub);

  // Materialise bridges from the DB and hand the manager to core BEFORE
  // install(): install_bridgeserv wires the relay hooks into it.
  svc::bridge::manager bm(database, cfg, hub);
  bm.load();
  core.set_bridge_manager(bm);

  core.install();
  hub.on_burst = [&core](svc::irc::link &l) { core.introduce_to(l); };
  hub.on_link_up = [](svc::irc::link &l) {
    svc::log::info("main", "link live: {}", l.remote_name());
  };

  // The network core hub (upstream to the maintree).
  {
    svc::irc::link_config lc;
    lc.host = svc::env::get("HUB_HOST").value_or("netcrave.chat");
    lc.port = svc::env::get("HUB_PORT").value_or("6697");
    lc.send_tls = svc::env::get_bool("HUB_TLS", true);
    lc.send_pass = svc::env::secret("LINK_SENDPASS").value_or("");
    lc.recv_pass = svc::env::secret("LINK_RECVPASS").value_or("");
    lc.reconnect = svc::env::get_bool("LINK_RETRY", true);
    lc.retry_min =
        std::chrono::milliseconds(1000LL * svc::env::get_int("LINK_RETRY_MIN", 5));
    lc.retry_max =
        std::chrono::milliseconds(1000LL * svc::env::get_int("LINK_RETRY_MAX", 300));
    if (lc.retry_max < lc.retry_min)
      lc.retry_max = lc.retry_min;
    if (lc.send_pass.empty())
      svc::log::warn("main",
                     "no LINK_SENDPASS in environment; the network hub link "
                     "may reject this daemon");
    hub.add_uplink(std::move(lc));
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  svc::log::info("main", "inspi services started ({} / {}, {} bridges)",
                 hcfg.server_name, hcfg.server_sid, bm.list().size());

  reactor.run();
  svc::log::info("main", "bye");
  return 0;
}