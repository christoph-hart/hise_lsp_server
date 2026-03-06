# hise-lsp

A native LSP (Language Server Protocol) proxy that bridges AI coding agents to HISE's diagnostic engine. When you edit a `.js` file, the LSP catches API hallucinations, wrong argument counts, type mismatches, and audio-thread safety violations — with fuzzy "did you mean?" suggestions — before you even compile.

## How it works

```
Agent edits .js file → save → hise-lsp (stdio) → POST /api/diagnose_script → HISE
                                                ← diagnostics (JSON)
                     ← publishDiagnostics ←
```

The binary is a stateless forwarder: it receives LSP notifications over stdio, calls HISE's REST API on localhost, and maps the response back to standard LSP `publishDiagnostics`. No caching, no file tracking — every save triggers a fresh diagnosis.

## Prerequisites

- **HISE** must be running with the REST API enabled (default port 1900)
- A HISE project with external `.js` script files included via `include("filename")`

## Quick start

### Precompiled binaries

Precompiled binaries are in `bin/`:

| Platform | Path |
|----------|------|
| Windows | `bin/windows/hise-lsp.exe` |
| macOS | `bin/macos/hise-lsp` |
| Linux | `bin/linux/hise-lsp` |

### Agent configuration

#### Claude Code

Install the plugin at project scope:

```bash
claude --plugin-dir tools/hise_lsp_server/plugin
```

The plugin's `.lsp.json` maps `.js` files to `hisescript` and points to the precompiled binary. Claude Code spawns it automatically.

#### OpenCode

Add to your HISE project's `opencode.json` (place it in the project root, e.g. `extras/demo_project/opencode.json`):

```json
{
  "$schema": "https://opencode.ai/config.json",
  "lsp": {
    "hisescript": {
      "command": ["/absolute/path/to/hise_lsp_server/bin/macos/hise-lsp", "--strict", "--flat-suggestions"],
      "extensions": [".js"]
    }
  }
}
```

> **Important — use absolute paths and project-level config:**
>
> - **Absolute paths:** OpenCode does not support `{env:VAR}` substitution in LSP `command` arrays. Use the full absolute path to the binary.
> - **Project-level config:** This config must go in your HISE project directory, **not** in the global OpenCode config (`~/.local/share/opencode/opencode.json`). A global config would override the `.js` extension mapping for the built-in JavaScript/TypeScript LSP across all your projects. By scoping it per-project, only `.js` files in your HISE project are diagnosed as HISEScript.
> - **gitignore it:** Since the path is both machine-specific and platform-specific (`bin/macos/`, `bin/windows/`, `bin/linux/`), add `opencode.json` to your `.gitignore`. Each developer creates their own locally.

Replace the binary path with the correct one for your platform:

| Platform | Binary path |
|----------|-------------|
| Windows | `.../bin/windows/hise-lsp.exe` |
| macOS | `.../bin/macos/hise-lsp` |
| Linux | `.../bin/linux/hise-lsp` |

See `config/opencode.json.example` for a complete example.

> **Why both flags?** As of early 2026, OpenCode's LSP integration has two limitations that affect AI agents:
>
> 1. **Only Error-severity diagnostics are surfaced** — warnings, info, and hints are silently filtered before reaching the agent. `--strict` promotes all severities to Error with a `[warning]`/`[info]`/`[hint]` prefix in the message text.
> 2. **The diagnostic `data` field is ignored** — OpenCode only reads the `message` string, so structured suggestions (e.g., fuzzy-matched method names) are invisible to the agent. `--flat-suggestions` coalesces them into the message as `". Use: suggestion"`.
>
> These are general OpenCode limitations that affect all LSPs, not just hise-lsp. This may change in future OpenCode versions — check their release notes and remove the flags if they become unnecessary.

#### Platform paths

Replace `bin/windows/hise-lsp.exe` with `bin/macos/hise-lsp` or `bin/linux/hise-lsp` as appropriate.

## Command line options

| Option | Default | Description |
|--------|---------|-------------|
| `--port <N>` | 1900 | HISE REST API port |
| `--host <H>` | localhost | HISE REST API host |
| `--strict` | off | Promote all diagnostics to Error severity (see below) |
| `--flat-suggestions` | off | Coalesce suggestions into the message string (see below) |
| `--test` | — | Run built-in unit tests and exit |

### Strict mode

Agent platforms like OpenCode only surface **Error** severity diagnostics to the AI agent — warnings, info, and hints are silently filtered. The `--strict` flag promotes all diagnostic severities to Error (severity 1) so that every diagnostic reaches the agent. The original severity is preserved as a message prefix:

```
[warning] [CallScope] *.showControl (unsafe)
[hint] Consider using 'local' instead of 'reg' in this scope
```

Errors (already severity 1) are passed through unchanged with no prefix.

### Flat suggestions

HISE diagnostics can include structured suggestions (e.g., fuzzy-matched method names). In standard LSP mode these are returned in the diagnostic's `data` field, which spec-compliant clients can read and display.

Some agent platforms don't read the `data` field at all — only the `message` string is visible to the AI agent. The `--flat-suggestions` flag coalesces suggestions into the message string:

```
Console.prins not found. Use: print
Console.prinsts not found. Use: print or printer
g.drawText is deprecated. Use: g.drawAlignedText() for proper text placement
```

Without the flag, the same diagnostics would show only the base message (e.g., `"Console.prins not found"`) and the suggestions would be in the structured `data` field.

This flag is independent of `--strict` — they can be used separately or together.

### Recommended configuration for AI agents

Most AI agent platforms need both flags:

```json
{
  "lsp": {
    "hisescript": {
      "command": ["/path/to/hise_lsp_server/bin/macos/hise-lsp", "--strict", "--flat-suggestions"],
      "extensions": [".js"]
    }
  }
}
```

**Omit both flags for normal editors** (VS Code, Neovim, etc.) so diagnostics render with the correct severities and structured suggestion data.

Example with custom port:

```json
{
  "lsp": {
    "hisescript": {
      "command": ["/path/to/hise_lsp_server/bin/macos/hise-lsp", "--strict", "--flat-suggestions", "--port", "2000"],
      "extensions": [".js"]
    }
  }
}
```

## Building from source

1. Open `cpp/hise-lsp.jucer` in Projucer
2. Generate the IDE project (Visual Studio / Xcode)
3. Build the **Release** configuration
4. Copy the binary to `bin/<platform>/`

**Requirements:** JUCE (the repo assumes it's a submodule of HISE at `tools/hise_lsp_server/`, so JUCE modules are at `../../../JUCE/modules` relative to the `.jucer` file).

## Diagnostic behavior

| Scenario | What you see |
|----------|-------------|
| HISE running, errors found | Mapped diagnostics with line, column, severity, suggestions |
| HISE running, clean file | Diagnostics cleared |
| HISE running, file not included | Error at line 1: "This file is not included in any HISE script processor..." |
| HISE not running | Error at line 1: "Cannot connect to HISE runtime (is HISE running on port 1900?)" |

## Troubleshooting

- **"Cannot connect to HISE runtime"** — Make sure HISE is running. Check the port number matches.
- **"This file is not included"** — The `.js` file must be `include()`d in a HISE script processor and compiled (F5) at least once.
- **No diagnostics appearing** — Verify the LSP binary path in your agent config is correct and the binary is executable.
- **Diagnostics from wrong LSP** — If you see TypeScript/JavaScript diagnostics alongside HISE diagnostics, you can disable the built-in JS LSP in your agent's config.

## LSP capabilities

Minimal surface — only document sync:

- `textDocumentSync.openClose`: true
- `textDocumentSync.change`: 1 (full — receives entire document on every change)
- `textDocumentSync.save`: true

Both `textDocument/didChange` and `textDocument/didSave` trigger diagnostics. This ensures compatibility with agents that use `didChange` (OpenCode) and agents that use `didSave` (Claude Code, VS Code).

No hover, completion, goto, formatting, or any other LSP feature. Just diagnostics.
