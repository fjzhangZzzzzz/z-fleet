#include "packager.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  CLI::App app{"z-fleet packager"};
  app.require_subcommand(1);

  std::string component_arg;
  std::string version_arg;
  std::string binary_arg;
  std::string output_dir_arg;
  std::string min_installer_version_arg = "0.1.0";
  bool force = false;

  auto* pack_dir_command =
      app.add_subcommand("pack-dir", "Create a package directory");
  pack_dir_command->add_option("--component", component_arg, "Component name")
      ->required();
  pack_dir_command->add_option("--version", version_arg, "Package version")
      ->required();
  pack_dir_command->add_option("--binary", binary_arg, "Built binary path")
      ->required();
  pack_dir_command->add_option("--output-dir", output_dir_arg,
                               "Output directory")
      ->required();
  pack_dir_command
      ->add_option("--min-installer-version", min_installer_version_arg,
                   "Minimum installer version");
  pack_dir_command->add_flag("--force", force, "Overwrite existing package");

  try {
    app.parse(argc, argv);

    if (!*pack_dir_command) {
      throw std::invalid_argument("missing pack-dir subcommand");
    }

    const auto result =
        zfleet::packager::PackDir(zfleet::packager::PackDirOptions{
            .component = component_arg,
            .version = version_arg,
            .binary_path = binary_arg,
            .output_dir = output_dir_arg,
            .min_installer_version = min_installer_version_arg,
            .force = force,
        });

    std::cout << result.package_dir.string() << '\n';
    return 0;
  } catch (const CLI::ParseError& ex) {
    const auto exit_code = app.exit(ex);
    return exit_code == 0 ? 0 : 2;
  } catch (const std::invalid_argument& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
