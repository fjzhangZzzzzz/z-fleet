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
  std::string platform_arg;
  std::string arch_arg;
  std::string build_type_arg;
  std::string payload_dir_arg;
  std::string entry_arg;
  std::string output_dir_arg;
  std::string min_installer_version_arg = "0.1.0";
  bool zip = false;
  bool force = false;

  auto* pack_command = app.add_subcommand("pack", "Create a package");
  pack_command->add_option("--component", component_arg, "Component name")
      ->required();
  pack_command->add_option("--version", version_arg, "Package version")
      ->required();
  pack_command->add_option("--platform", platform_arg, "Target platform")
      ->required();
  pack_command->add_option("--arch", arch_arg, "Target architecture")
      ->required();
  pack_command->add_option("--build-type", build_type_arg,
                           "Build type (debug or release)")
      ->required();
  pack_command->add_option("--payload-dir", payload_dir_arg,
                           "Payload directory")
      ->required();
  pack_command->add_option("--entry", entry_arg,
                           "Component entry path under payload directory")
      ->required();
  pack_command->add_option("--output-dir", output_dir_arg, "Output directory")
      ->required();
  pack_command
      ->add_option("--min-installer-version", min_installer_version_arg,
                   "Minimum installer version");
  pack_command->add_flag("--zip", zip, "Create a .zip package");
  pack_command->add_flag("--force", force, "Overwrite existing output");

  try {
    app.parse(argc, argv);

    if (!*pack_command) {
      throw std::invalid_argument("missing subcommand");
    }

    const auto result = zfleet::packager::Pack(zfleet::packager::PackOptions{
        .component = component_arg,
        .version = version_arg,
        .platform = platform_arg,
        .arch = arch_arg,
        .build_type = build_type_arg,
        .payload_dir = payload_dir_arg,
        .entry_path = entry_arg,
        .output_dir = output_dir_arg,
        .min_installer_version = min_installer_version_arg,
        .archive = zip,
        .force = force,
    });

    std::cout << result.package_path.string() << '\n';
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
