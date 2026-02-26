# AGENTS.md — hise-lsp

## What this is

A stateless LSP (Language Server Protocol) proxy binary. Receives JSON-RPC over stdio from an AI coding agent, forwards file paths to HISE's `POST /api/diagnose_script` REST endpoint, maps the response to LSP `publishDiagnostics` notifications.

## Architecture

- **Single source file:** `cpp/Source/Main.cpp` (~350 lines)
- **Stateless:** No file tracking, no caching. Every `didOpen`/`didSave` triggers a fresh HTTP call to HISE.
- **stdio transport:** JSON-RPC with `Content-Length` framing on stdin/stdout. Logging goes to stderr.

## Build system

- **Projucer** project: `cpp/hise-lsp.jucer`
- **JUCE modules:** `juce_core` + `juce_data_structures` + `juce_events`
- **JUCE module path:** `../../../JUCE/modules` (assumes this repo is a submodule of HISE at `tools/hise_lsp_server/`)
- **Build:** Open `.jucer` in Projucer → generate VS/Xcode project → build Release
- **Binary output:** Copy to `bin/{platform}/` and commit

**Never modify `cpp/Builds/` or `cpp/JuceLibraryCode/`** — these are Projucer-generated.

## Code style

- C++17, JUCE framework, `using namespace juce;`
- Tabs for indentation, Allman brace style
- `MemoryOutputStream` for string building (not `String` concatenation — performance)
- `DynamicObject::Ptr` for JSON construction
- All output to user via stderr (`log()`), stdout is the LSP channel only

## REST API dependency

The binary calls `POST /api/diagnose_script` on HISE's REST API (default `localhost:1900`). See `guidelines/api/rest-api.md` in the main HISE repo for the endpoint spec.

## Key behaviors

- `didOpen` / `didSave` / `didChange` → diagnose and publish
- `didClose` → clear diagnostics
- HISE not running → synthetic Error at line 1
- File not included in HISE → synthetic Error at line 1
- Clean file → empty diagnostics (clears previous)
- `--strict` flag → promotes all severities to Error (1), prefixes message with original severity (e.g. `[warning]`)

## Files

| File | Purpose |
|------|---------|
| `cpp/Source/Main.cpp` | Full LSP implementation |
| `cpp/hise-lsp.jucer` | Projucer project |
| `bin/` | Precompiled binaries (committed) |
| `plugin/` | Claude Code plugin wrapper |
| `config/` | Example configs for OpenCode, Crush |
