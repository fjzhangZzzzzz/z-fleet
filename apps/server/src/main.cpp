#include "config.h"
#include "database.h"
#include "http2_connection_registry.h"
#include "http2_control_server.h"
#include "http2_control_service.h"
#include "management_http_server.h"

#include "zfleet/core/log.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>

namespace {

constexpr char kComponentRootEnvVar[] = "ZFLEET_COMPONENT_ROOT";

std::filesystem::path DetectInstallDir() {
  if (const char* value = std::getenv(kComponentRootEnvVar);
      value != nullptr && value[0] != '\0') {
    return std::filesystem::path(value);
  }
  return std::filesystem::current_path();
}

std::filesystem::path DetectReleaseWebStaticDir(const char* executable_path) {
  const auto executable =
      std::filesystem::absolute(executable_path == nullptr ? "" : executable_path)
          .lexically_normal();
  return executable.parent_path().parent_path() / "share" / "web";
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"z-fleet server"};

  std::string config_path_arg;
  std::string control_listen_arg;
  std::string management_listen_arg;
  std::string database_path_arg;
  std::string package_repository_arg;
  std::string web_static_dir_arg;
  std::string log_level_arg;
  app.add_option("-c,--config", config_path_arg, "Path to server config file");
  app.add_option("--control-listen", control_listen_arg,
                 "Override server HTTP/2 control listen address");
  app.add_option("--management-listen", management_listen_arg,
                 "Override server Web management listen address");
  app.add_option("--database-path", database_path_arg,
                 "Override server database path");
  app.add_option("--package-repository", package_repository_arg,
                 "Override server package repository path");
  app.add_option("--web-static-dir", web_static_dir_arg,
                 "Override server Web static asset directory");
  app.add_option("--log-level", log_level_arg, "Override server log level");

  CLI11_PARSE(app, argc, argv);

  try {
    const auto install_dir = DetectInstallDir();
    const auto config_path =
        config_path_arg.empty()
            ? zfleet::server::DefaultConfigPath(
                  std::optional<std::filesystem::path>{install_dir})
            : std::filesystem::path{config_path_arg};
    auto config = std::filesystem::exists(config_path)
                      ? zfleet::server::LoadConfig(
                            std::optional<std::filesystem::path>{config_path})
                      : zfleet::server::ServerConfig{};
    config.install_dir = install_dir;
    if (!database_path_arg.empty()) {
      config.database_path = database_path_arg;
    }
    if (!control_listen_arg.empty()) {
      config.control_listen = control_listen_arg;
    }
    if (!management_listen_arg.empty()) {
      config.management_listen = management_listen_arg;
    }
    if (!package_repository_arg.empty()) {
      config.package_repository = package_repository_arg;
    }
    if (!web_static_dir_arg.empty()) {
      config.web_static_dir = web_static_dir_arg;
    }
    if (!log_level_arg.empty()) {
      config.log.level = zfleet::core::log::ParseLevel(log_level_arg);
    }
    zfleet::server::SaveConfig(config, config_path);
    zfleet::server::ResolveConfigPaths(&config);
    if (config.web_static_dir.empty()) {
      config.web_static_dir =
          DetectReleaseWebStaticDir(argc > 0 ? argv[0] : nullptr);
    }

    zfleet::core::log::Init(config.log);
    const auto logger = zfleet::core::log::Component("server").With(
        {{"control_listen", config.control_listen},
         {"database_path", config.database_path.string()}});

    zfleet::server::ServerDatabase database(config.database_path);
    database.Initialize();
    const zfleet::server::Http2ControlService control_service(&database);
    zfleet::server::Http2ConnectionRegistry connection_registry;
    zfleet::server::Http2ControlServer control_server(config.control_listen,
                                                      &database,
                                                      &control_service,
                                                      &connection_registry);
    zfleet::server::ManagementHttpServer management_server(
        config.management_listen,
        &database,
        config.package_repository,
        config.web_static_dir);
    management_server.Start();

    ZFLOG_INFO(logger,
               "{} server {} protocol {} on {} schema_version={} control_listen={} management_listen={}",
               zfleet::core::project_name(),
               zfleet::core::version(),
               zfleet::protocol::protocol_version(),
               zfleet::platform::os_name(),
               database.schema_version(),
               config.control_listen,
               config.management_listen);
    control_server.Run();
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
