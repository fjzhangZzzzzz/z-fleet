#include "installer.h"
#include "json_codec.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  CLI::App app{"z-fleet installer"};
  app.require_subcommand(1);

  std::string root_arg;
  std::string package_arg;
  auto* apply_command = app.add_subcommand("apply", "Apply a package directory");
  apply_command->add_option("--root", root_arg, "Install root")->required();
  apply_command->add_option("--package", package_arg, "Package directory")
      ->required();

  std::string status_root_arg;
  std::string component_arg;
  auto* status_command =
      app.add_subcommand("status", "Inspect active component status");
  status_command->add_option("--root", status_root_arg, "Install root")
      ->required();
  status_command
      ->add_option("--component", component_arg, "Component name")
      ->required()
      ->check(CLI::IsMember({"agent", "server", "installer"}));

  std::string rollback_root_arg;
  std::string rollback_component_arg;
  auto* rollback_command =
      app.add_subcommand("rollback", "Rollback to the previous component release");
  rollback_command->add_option("--root", rollback_root_arg, "Install root")
      ->required();
  rollback_command
      ->add_option("--component", rollback_component_arg, "Component name")
      ->required()
      ->check(CLI::IsMember({"agent", "server", "installer"}));

  CLI11_PARSE(app, argc, argv);

  try {
    if (*apply_command) {
      const auto result =
          zfleet::installer::ApplyPackage(root_arg, package_arg);
      if (!result.ok) {
        std::cerr << result.message << '\n';
        return 1;
      }
      return 0;
    }

    if (*rollback_command) {
      const auto result =
          zfleet::installer::RollbackComponent(rollback_root_arg,
                                               rollback_component_arg);
      if (!result.ok) {
        std::cerr << result.message << '\n';
        return 1;
      }
      return 0;
    }

    const auto result =
        zfleet::installer::GetStatus(status_root_arg, component_arg);
    std::cout << zfleet::installer::SerializeStatusResult(result) << '\n';
    return result.state == "corrupt" ? 1 : 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
