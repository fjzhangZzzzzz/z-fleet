# Repository Guidelines

## Project Structure & Module Organization

`z-fleet` is a C++20 monorepo built with CMake and vcpkg manifest mode. Application entry points live in `apps/agent/` and `apps/server/`. Shared code belongs in `libs/core/`, `libs/protocol/`, and `libs/platform/`, each with `src/`, `include/`, and `tests/`. Cross-component tests go in `tests/integration/`. Design and maintenance docs live in `docs/`, with ADRs under `docs/adr/`.

## Build, Test, and Development Commands

Use the repository scripts so local and CI behavior stay aligned:

- `./scripts/build.sh linux-debug`: bootstrap vcpkg, configure CMake, and build the Linux debug preset.
- `./scripts/build.sh linux-release`: build the Linux release preset.
- `./scripts/test.sh linux-debug`: run `ctest` for the Linux debug preset.
- `cmake --preset linux-debug`: configure only, useful when iterating on build files.

Windows builds use the matching `windows-debug` and `windows-release` presets from Git Bash.

## Coding Style & Naming Conventions

Follow the existing C++ style in the repo: 2-space indentation, braces on the same line, and small translation units. Use project namespaces such as `zfleet::core` and keep public headers under `include/zfleet/...`. Source and test files use lowercase snake_case, for example `version.cpp` and `version_test.cpp`. Keep document filenames stable: core docs use English names like `architecture.md`, and ADRs use `NNNN-short-name.md`.

## Testing Guidelines

Tests are currently lightweight executable checks wired into CTest. Add unit tests beside the owning module under `libs/*/tests/` or `apps/*/tests/`; use `tests/integration/` only for end-to-end flows. Name test files with the `_test.cpp` suffix. Run at least `./scripts/test.sh linux-debug` before opening a PR.

## Commit & Pull Request Guidelines

Recent history uses short conventional prefixes, for example `feat: 重构文档组织结构` and `feat: 项目最小骨架搭建`. Keep commit subjects imperative and scoped; prefer `feat:`, `fix:`, `docs:`, `build:`, or `test:`. PRs should describe the problem, summarize the change, list validation commands, and link the relevant issue or ADR. Include screenshots only when UI work is introduced.

## Documentation & Security Notes

Documentation is maintained under `docs/README.md`; keep prose in Chinese unless English is required for identifiers, commands, paths, or external standards. Do not introduce shell execution, write-capable tasks, or update paths that bypass the project’s documented safety model in `docs/security.md`.
