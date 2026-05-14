#include "config.h"
#include "database.h"

#include "zfleet/core/log.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <optional>
#include <sstream>

int main(int argc, char** argv) {
  CLI::App app{"z-fleet server"};

  std::string config_path_arg;
  std::string database_path_arg;
  std::string listen_arg;
  std::string log_level_arg;
  app.add_option("--config", config_path_arg, "Path to server config file");
  app.add_option("--database-path", database_path_arg,
                 "Override server database path");
  app.add_option("--listen", listen_arg, "Override server listen address");
  app.add_option("--log-level", log_level_arg, "Override server log level");

  CLI11_PARSE(app, argc, argv);

  try {
    const auto config_path =
        config_path_arg.empty()
            ? std::optional<std::filesystem::path>{}
            : std::optional<std::filesystem::path>{config_path_arg};
    auto config = zfleet::server::LoadConfig(config_path);
    if (!database_path_arg.empty()) {
      config.database_path = database_path_arg;
    }
    if (!listen_arg.empty()) {
      config.listen = listen_arg;
    }
    if (!log_level_arg.empty()) {
      config.log.level = zfleet::core::log::ParseLevel(log_level_arg);
    }

    zfleet::core::log::Init(config.log);
    const auto logger = zfleet::core::log::Component("server").With(
        {{"listen", config.listen},
         {"database_path", config.database_path.string()}});

    zfleet::server::ServerDatabase database(config.database_path);
    database.Initialize();

    std::ostringstream message;
    message << zfleet::core::project_name() << " server "
            << zfleet::core::version() << " protocol "
            << zfleet::protocol::protocol_version() << " on "
            << zfleet::platform::os_name() << " schema_version="
            << database.schema_version();
    zfleet::core::log::Write(
        zfleet::core::log::Level::kInfo, logger, message.str());
    zfleet::core::log::Shutdown();
    return 0;
  } catch (const std::exception& ex) {
    zfleet::core::log::Write(zfleet::core::log::Level::kError,
                             zfleet::core::log::Component("server"),
                             std::string("server startup failed: ") + ex.what());
    zfleet::core::log::Shutdown();
    return 1;
  }
}
