#include "packager.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  CLI::App app{"z-fleet packager"};
  app.require_subcommand(1);

  std::string pack_dir_component_arg;
  std::string pack_dir_version_arg;
  std::string pack_dir_binary_arg;
  std::string pack_dir_output_dir_arg;
  std::string pack_dir_min_installer_version_arg = "0.1.0";
  bool pack_dir_force = false;

  std::string pack_archive_component_arg;
  std::string pack_archive_version_arg;
  std::string pack_archive_binary_arg;
  std::string pack_archive_output_dir_arg;
  std::string pack_archive_min_installer_version_arg = "0.1.0";
  bool pack_archive_force = false;

  std::string archive_dir_package_arg;
  std::string archive_dir_archive_arg;
  bool archive_dir_force = false;

  auto* pack_dir_command =
      app.add_subcommand("pack-dir", "Create a package directory");
  pack_dir_command->add_option("--component", pack_dir_component_arg,
                               "Component name")
      ->required();
  pack_dir_command->add_option("--version", pack_dir_version_arg,
                               "Package version")
      ->required();
  pack_dir_command->add_option("--binary", pack_dir_binary_arg,
                               "Built binary path")
      ->required();
  pack_dir_command->add_option("--output-dir", pack_dir_output_dir_arg,
                               "Output directory")
      ->required();
  pack_dir_command
      ->add_option("--min-installer-version",
                   pack_dir_min_installer_version_arg,
                   "Minimum installer version");
  pack_dir_command->add_flag("--force", pack_dir_force,
                             "Overwrite existing package");

  auto* pack_archive_command =
      app.add_subcommand("pack-archive", "Create a .zip archive from binary");
  pack_archive_command
      ->add_option("--component", pack_archive_component_arg,
                   "Component name")
      ->required();
  pack_archive_command->add_option("--version", pack_archive_version_arg,
                                   "Package version")
      ->required();
  pack_archive_command->add_option("--binary", pack_archive_binary_arg,
                                   "Built binary path")
      ->required();
  pack_archive_command->add_option("--output-dir", pack_archive_output_dir_arg,
                                   "Output directory")
      ->required();
  pack_archive_command
      ->add_option("--min-installer-version",
                   pack_archive_min_installer_version_arg,
                   "Minimum installer version");
  pack_archive_command->add_flag("--force", pack_archive_force,
                                 "Overwrite existing archive");

  auto* archive_dir_command =
      app.add_subcommand("archive-dir",
                         "Create a .zip archive from a package directory");
  archive_dir_command->add_option("--package", archive_dir_package_arg,
                                  "Package directory")
      ->required();
  archive_dir_command->add_option("--archive", archive_dir_archive_arg,
                                  "Archive path")
      ->required();
  archive_dir_command->add_flag("--force", archive_dir_force,
                                "Overwrite existing archive");

  try {
    app.parse(argc, argv);

    if (!*pack_dir_command) {
      if (*pack_archive_command) {
        const auto result = zfleet::packager::PackArchive(
            zfleet::packager::PackArchiveOptions{
                .component = pack_archive_component_arg,
                .version = pack_archive_version_arg,
                .binary_path = pack_archive_binary_arg,
                .output_dir = pack_archive_output_dir_arg,
                .min_installer_version =
                    pack_archive_min_installer_version_arg,
                .force = pack_archive_force,
            });
        std::cout << result.archive_path.string() << '\n';
        return 0;
      }

      if (*archive_dir_command) {
        const auto result = zfleet::packager::ArchiveDir(
            zfleet::packager::ArchiveDirOptions{
                .package_dir = archive_dir_package_arg,
                .archive_path = archive_dir_archive_arg,
                .force = archive_dir_force,
            });
        std::cout << result.archive_path.string() << '\n';
        return 0;
      }

      throw std::invalid_argument("missing subcommand");
    }

    const auto result =
        zfleet::packager::PackDir(zfleet::packager::PackDirOptions{
            .component = pack_dir_component_arg,
            .version = pack_dir_version_arg,
            .binary_path = pack_dir_binary_arg,
            .output_dir = pack_dir_output_dir_arg,
            .min_installer_version = pack_dir_min_installer_version_arg,
            .force = pack_dir_force,
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
