# ESP-MeteoRadar Project Rules & Agent Workflow

## Autonomy & Execution
- **Autonomous Execution**: Proactively apply code edits, file changes, and compilation commands (`pio run`) directly without asking for confirmation for routine changes or creating blocking approval gates.
- **Compilation**: After making code modifications in `src/` or `include/`, verify the build by running `pio run` to keep `merged-firmware.bin` synchronized.
- **Git Commits & Releases**: Do NOT automatically run `git commit` after every change. Instead, keep changes in the working tree and suggest a commit and release only when a logical batch of features/fixes is ready.

## Code Quality & Standards
- Maintain strict memory isolation on ESP32-C3 (e.g., release display canvas/sprites during TLS handshakes and large network requests).
- Use streaming/chunked parsing for large JSON payloads (>1KB) instead of loading entire strings into RAM.
- Keep the local web server responsive, lightweight, and modern.
- Preserve comments, code structure, and formatting.

## Communication
- Respond concisely, directly, and in Slovak.
