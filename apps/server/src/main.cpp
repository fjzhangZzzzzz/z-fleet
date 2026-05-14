#include "config.h"
#include "database.h"

#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <iostream>
#include <optional>

int main(int argc, char** argv) {
  CLI::App app{"z-fleet server"};

  std::string config_path_arg;
  std::string database_path_arg;
  std::string listen_arg;
  app.add_option("--config", config_path_arg, "Path to server config file");
  app.add_option("--database-path", database_path_arg,
                 "Override server database path");
  app.add_option("--listen", listen_arg, "Override server listen address");

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

    zfleet::server::ServerDatabase database(config.database_path);
    database.Initialize();

    std::cout << zfleet::core::project_name() << " server "
              << zfleet::core::version() << " protocol "
              << zfleet::protocol::protocol_version() << " on "
              << zfleet::platform::os_name() << '\n'
              << "listen=" << config.listen << '\n'
              << "database_path=" << database.database_path().string() << '\n'
              << "schema_version=" << database.schema_version() << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "server startup failed: " << ex.what() << '\n';
    return 1;
  }
}
