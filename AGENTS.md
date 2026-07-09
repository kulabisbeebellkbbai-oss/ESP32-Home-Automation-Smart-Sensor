# Repository Guidelines

## Project Structure & Module Organization

Keep source code in `src/`, tests in `tests/`, assets in `assets/`, and contributor notes in `docs/` as the project grows. Keep `.agents/` and `.codex/` for local agent metadata.

## Build, Test, and Development Commands

- `pio run` - build the ESP32 firmware and resolve PlatformIO dependencies.
- `pio run -t upload` - flash the connected ESP32 board.
- `pio device monitor -b 115200` - open the serial monitor for boot and sensor diagnostics.
- `cd android && gradle --no-daemon assembleDebug` - build the Android BLE provisioning app.

## Coding Style & Naming Conventions

Follow the conventions of the language and framework used in this project. Prefer descriptive module names and small, focused files.

## Testing Guidelines

Add tests with new behavior and mirror the source layout where practical. Name tests after the behavior they verify.

## Commit & Pull Request Guidelines

Use clear imperative commit messages. Pull requests should include a short summary, test results, and relevant screenshots for UI changes.
