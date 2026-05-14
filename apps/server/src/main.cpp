#include "config.h"
#include "database.h"
#include "http_handler.h"
#include "http_server.h"

#include "zfleet/core/log.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <optional>

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
    const zfleet::server::HttpHandler handler(&database);
    zfleet::server::HttpServer server(config.listen, &handler);

    ZFLOG_INFO(logger,
               "{} server {} protocol {} on {} schema_version={}",
               zfleet::core::project_name(),
               zfleet::core::version(),
               zfleet::protocol::protocol_version(),
               zfleet::platform::os_name(),
               database.schema_version());
    server.Run();
    zfleet::core::log::Shutdown();
    return 0;
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server"),
                "server startup failed: {}",
                ex.what());
    zfleet::core::log::Shutdown();
    return 1;
  }
}
