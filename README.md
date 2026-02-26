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

Add to your project's `opencode.json`:

```json
{
  "lsp": {
    "hisescript": {
      "command": ["tools/hise_lsp_server/bin/windows/hise-lsp.exe"],
      "extensions": [".js"]
    }
  }
}
```

See `config/opencode.json.example` for a complete example.

#### Crush

Add to your project's `crush.json`:

```json
{
  "lsp": {
    "hisescript": {
      "command": "tools/hise_lsp_server/bin/windows/hise-lsp.exe",
      "args": []
    }
  }
}
```

See `config/crush.json.example` for a complete example.

#### Platform paths

Replace `bin/windows/hise-lsp.exe` with `bin/macos/hise-lsp` or `bin/linux/hise-lsp` as appropriate.

## Command line options

| Option | Default | Description |
|--------|---------|-------------|
| `--port <N>` | 1900 | HISE REST API port |
| `--host <H>` | localhost | HISE REST API host |

Environment variables `HISE_LSP_PORT` and `HISE_LSP_HOST` are also supported (command line args take priority).

Example with custom port:

```json
{
  "lsp": {
    "hisescript": {
      "command": ["tools/hise_lsp_server/bin/windows/hise-lsp.exe", "--port", "2000"],
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
- `textDocumentSync.change`: 0 (none — no `didChange`)
- `textDocumentSync.save`: true

No hover, completion, goto, formatting, or any other LSP feature. Just diagnostics.
