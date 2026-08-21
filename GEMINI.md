# Project Instructions & Autonomy Rules

## Autonomy & Decision Making
- **Full Autonomous Mode**: Always act autonomously. Do not ask for confirmation or interactive approval for standard tasks, code modifications, debugging, file operations, or running development/build commands (`platformio run`, scripts, git checks, etc.).
- **Proactive Execution**: Directly implement solutions, apply edits, and verify results without unnecessary round-trips or asking permission to proceed with obvious next steps.
- **Verification**: Automatically verify code changes by running the appropriate build (`C:\Users\radod\.platformio\penv\Scripts\platformio.exe run` or `pio run`) after making modifications.
- **Problem Solving**: If an error occurs or a build fails, inspect the error output, diagnose the root cause, and autonomously apply the fix and re-verify.

## Directory Structure & Clean Root Policy
- **Root Configuration Files**: Only standard tool configurations belong in root: `platformio.ini`, `partitions.csv`, `README.md`, `LICENSE`, `GEMINI.md`, `AGENTS.md`.
- **Clean Root Directory**: NEVER place ad-hoc helper files, temporary logs, scratch scripts, or redundant assets into the repository root.
- **Appropriate Subdirectories**: Always organize files into their dedicated folders:
  - `scripts/` – Build scripts, python utilities, post-processing tools
  - `include/` – Header files and configurations
  - `src/` – Core implementation C++ files
  - `data/` – Static assets, images, SPIFFS files
  - `.agents/` – Agent workflow rules and instructions
  - `.vscode/` – IDE configurations and recommended extensions

## Project Context
- **Project**: ESP-MeteoRadar (ESP32-C3 Super Mini + GC9A01 240x240 round display)
- **Framework**: Arduino / PlatformIO
- **Build tool**: PlatformIO CLI (`pio run` / PlatformIO penv)
