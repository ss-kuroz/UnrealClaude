# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**UnrealClaude** is an Unreal Engine 5.7 Editor plugin that integrates Claude Code CLI into the editor via an MCP (Model Context Protocol) bridge. It consists of a C++ UE plugin (`UnrealClaude/`) and a Node.js MCP bridge (`UnrealClaude/Resources/mcp-bridge/`, a git submodule). The C++ side exposes an HTTP REST API (port 3000) that the Node.js bridge consumes to provide MCP-compatible tools to Claude Code and other MCP clients.

## Build Commands

### Plugin (C++)
Build using Unreal's UAT. `build.bat` contains the author's local paths and should not be run directly; adapt the paths below instead.

**Windows:**
```bash
"<UE_PATH>/Engine/Build/BatchFiles/RunUAT.bat" BuildPlugin -Plugin="<PLUGIN_PATH>/UnrealClaude.uplugin" -Package="<OUTPUT_PATH>" -TargetPlatforms=Win64
```

**Linux/macOS:**
```bash
"<UE_PATH>/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin -Plugin="<PLUGIN_PATH>/UnrealClaude.uplugin" -Package="<OUTPUT_PATH>" -TargetPlatforms=Linux  # or Mac
```

After building, copy the output to your project's `Plugins/` directory (recommended) or the engine's `Engine/Plugins/Marketplace/` directory.

### MCP Bridge (Node.js)
The bridge is a git submodule. If you cloned without `--recurse-submodules`, run `git submodule update --init` first.

```bash
cd UnrealClaude/Resources/mcp-bridge
npm install
npm test                  # Run full Vitest suite once
npm run test:watch        # Run Vitest in watch mode
npm run test:coverage     # Run with coverage (thresholds: statements 90%, branches 85%, functions 90%, lines 90%)
```

To run a single test file: `npx vitest run tests/unit/<file>.test.js`

### C++ Automation Tests (in Unreal Editor console)
All C++ tests use `IMPLEMENT_SIMPLE_AUTOMATION_TEST` with flags `EditorContext | ProductFilter`. They are located in `UnrealClaude/Source/UnrealClaude/Private/Tests/`.

```
Automation RunTests UnrealClaude
Automation RunTests UnrealClaude.MCP.Tools            # Run MCP tool tests
Automation RunTests UnrealClaude.MCP.ParamValidator   # Run param validator tests
Automation RunTests UnrealClaude.MCP.Registry           # Run registry tests
```

## High-Level Architecture

### Core C++ Systems

The plugin is a single Editor module (`UnrealClaude`) loaded at `PostEngineInit`. Key systems:

1. **FClaudeCodeRunner** (`ClaudeCodeRunner.h/cpp`) — Async `FRunnable` that shells out to `claude -p` (Claude Code CLI print mode). Captures stdout/stderr via cross-platform pipes, parses NDJSON stream events, and includes a silence watchdog (60s threshold) to detect hung requests.
2. **FClaudeCodeSubsystem** (`ClaudeSubsystem.h/cpp`) — Singleton orchestrator. Builds system prompts (UE5.7 context + project context + custom `CLAUDE.md`), manages conversation history via `FClaudeSessionManager`, and delegates execution to `FClaudeCodeRunner`.
3. **FClaudeSessionManager** (`ClaudeSessionManager.h/cpp`) — Handles conversation persistence. Stores up to 50 exchanges, keeps the last 10 in the prompt context. Session file is saved to `Saved/UnrealClaude/`.
4. **SClaudeEditorWidget** (`ClaudeEditorWidget.h/cpp`) — Slate UI docked in the editor. Composes `SClaudeInputArea` (text input + image paste), `SClaudeToolbar`, and `SMarkdownWidget` (streaming display, tool-use grouping, code block rendering). Chat history is restored via `FClaudeSessionManager`.
5. **FUnrealClaudeMCPServer** (`MCP/UnrealClaudeMCPServer.h/cpp`) — HTTP server using Unreal's `HTTPServer` module. Exposes REST endpoints: `GET /mcp/tools`, `POST /mcp/tool/{name}`, `GET /mcp/status`. Delegates to `FMCPToolRegistry`.
6. **FMCPToolRegistry** (`MCP/MCPToolRegistry.h/cpp`) — Registry of all MCP tools. Each tool inherits `IMCPTool` (implements `GetInfo()` for schema + annotations, and `Execute()` for parameter validation + work). Built-in tools are auto-registered in `RegisterBuiltinTools()`. Uses `FMCPToolAnnotations` to hint tool behavior (ReadOnly, Modifying, Destructive) to LLM clients.
7. **FMCPTaskQueue** (`MCP/MCPTaskQueue.h/cpp`) — Background `FRunnable` task queue for long-running MCP tool execution. Max 4 concurrent tasks, 2-minute default timeout, 5-minute result retention. Tools can be submitted async with status polling and cancellation.
8. **FMCPToolBase** (`MCP/MCPToolBase.h/cpp`) — Base class for most tools. Provides `ValidateEditorContext`, actor finders, parameter extraction helpers (`ExtractRequiredString`, `ExtractVectorParam`, `ExtractRotatorParam`, etc.), validation wrappers (`ExtractAndValidate`), and JSON result builders (`BuildActorInfoJson`). New tools should inherit from this.
9. **FScriptExecutionManager** (`ScriptExecutionManager.h/cpp`) — Manages script execution pipeline with permission dialogs. Supports C++ (via Live Coding on Windows), Python, Console commands, and Editor Utility Blueprints. Scripts are written to `Saved/UnrealClaude/Scripts/` and history is persisted. Uses `IScriptExecutor` interface with per-type implementations.
10. **FProjectContextManager** (`ProjectContext.h/cpp`) — Gathers project context on demand: source modules, UCLASS definitions (up to 500 parsed, 30 formatted), level actors, asset counts, enabled plugins. Used to build the system prompt.
11. **Blueprint/Animation Subsystems** — Domain-specific editors for Blueprint manipulation:
    - `BlueprintEditor` / `BlueprintGraphEditor` — Variable/function/node operations.
    - `AnimGraphEditor` / `AnimStateMachineEditor` — Animation Blueprint state machines, transitions, and condition graphs.
    - `AnimAssetManager` / `AnimationBlueprintUtils` — Animation asset loading and Blueprint utilities.
    - `AnimNodePinUtils` — Pin connection and default-value helpers for anim graphs.

### MCP Bridge (Node.js)

The bridge is a standalone MCP server that talks to the C++ HTTP API:
- **`index.js`** — Entry point. Creates an MCP server via `@modelcontextprotocol/sdk`, connects to the C++ plugin via HTTP, and exposes tools.
- **`lib.js`** — HTTP client that calls the C++ REST API (`/mcp/tools`, `/mcp/tool/{name}`, `/mcp/status`).
- **`tool-router.js`** — Domain-based router for complex tools. Domain-specific operations (blueprint, anim, character, material, enhanced_input, asset) MUST be routed through `unreal_ue` instead of calling raw tools directly.
- **`context-loader.js`** — Dynamic UE 5.7 context system. Loads markdown files from `contexts/` and injects them into Claude's system prompt on demand via `unreal_get_ue_context`.

**Bridge environment variables:**
- `UNREAL_MCP_URL` — C++ plugin HTTP endpoint (default: `http://localhost:3000`)
- `INJECT_CONTEXT` — Auto-inject UE5 context on tool responses (default: `false`)
- `MCP_REQUEST_TIMEOUT_MS` — Request timeout (default: `30000`)

### Data Flow

```
User Input -> SClaudeEditorWidget -> FClaudeCodeSubsystem -> FClaudeCodeRunner -> claude CLI
                                                                    ^
                                                                    |
MCP Client -> Node.js Bridge -> HTTP -> FUnrealClaudeMCPServer -> FMCPToolRegistry -> FMCPTaskQueue (async) or direct execute
                                                                    |
                                                              Editor/Engine APIs
```

## Development Conventions

### C++ Code Standards
- Target **UE 5.7 only**. Do not use deprecated APIs from older engine versions.
- **Max 500 lines per file**, **max 50 lines per function**. Split large files into logical units.
- Public headers in `Public/`, implementation in `Private/`.
- Use `TObjectPtr<>` for UObject hard references, `TSoftObjectPtr<>` for soft references.
- UPROPERTY specifiers: `EditAnywhere, BlueprintReadWrite, Category = "..."` for editor-visible properties.
- UFUNCTION specifiers: `BlueprintCallable` for execution pins, `BlueprintPure` for no-side-effect getters.
- Centralize magic numbers in `UnrealClaudeConstants.h` (buffer sizes, limits, timeouts, diagram dimensions, etc.).

### MCP Tool Development
1. Create header in `Private/MCP/Tools/MCPTool_YourTool.h`, inherit from `FMCPToolBase`.
2. Implement `GetInfo()` with `FMCPToolInfo` (name, description, parameters, annotations).
3. Implement `Execute()` with parameter validation via base-class helpers (`ExtractRequiredString`, `ExtractAndValidate`, etc.) or `MCPParamValidator`.
4. Register in `MCPToolRegistry::RegisterBuiltinTools()`.
5. Add tests in `Private/Tests/` using `IMPLEMENT_SIMPLE_AUTOMATION_TEST` with flags `EditorContext | ProductFilter`.

### Tool Annotations
- `FMCPToolAnnotations::ReadOnly()` — Safe read-only tools (asset_search, get_level_actors, etc.)
- `FMCPToolAnnotations::Modifying()` — State-changing but reversible (spawn_actor, move_actor, blueprint_modify)
- `FMCPToolAnnotations::Destructive()` — Cannot be undone via MCP (delete_actors, cleanup_scripts)

### Parallel Execution Rules
- **Max 4 concurrent tasks** in the task queue. Keep parallel subagent count to **3 max**.
- **Read-only tools** can be called freely in parallel.
- **Per-object safe** modifying tools can run in parallel on *different* actors/assets. Never modify the same object from two calls simultaneously.
- **Sequential only**: `open_level`, `delete_actors`, `execute_script`, `cleanup_scripts`, `run_console_command` must run alone because `open_level` invalidates all references.

### Parameter Validation (Security)
Always validate in `Execute()` before doing work:
- Path validation: block `/Engine/`, `/Script/`, path traversal (`../`)
- Actor name validation: block special characters defined in `UnrealClaudeConstants::MCPValidation::DangerousChars`
- Console command validation: block dangerous commands (`quit`, `crash`, `shutdown`)
- Numeric validation: check for NaN, Infinity, and reasonable bounds (`UnrealClaudeConstants::NumericBounds::MaxCoordinateValue`)

### Commit Message Prefixes
- `feat:` — New MCP tools or features
- `fix:` — Bug fixes
- `test:` — Test additions or fixes
- `docs:` — Documentation updates
- `refactor:` — Code refactoring without behavior changes

## Ollama Integration

The plugin automatically injects Ollama environment variables into every `claude` child process, so **you do not need to set them before launching the editor**.

Injected variables:
- `ANTHROPIC_AUTH_TOKEN=ollama`
- `ANTHROPIC_API_KEY=` (empty)
- `ANTHROPIC_BASE_URL=http://localhost:11434`
- `ANTHROPIC_DEFAULT_OPUS_MODEL=kimi-k2.6:cloud`
- `ANTHROPIC_DEFAULT_SONNET_MODEL=kimi-k2.6:cloud`
- `ANTHROPIC_MODEL=kimi-k2.6:cloud`

Implementation details:
- **Wrapper script** (`FClaudeCodeRunner::CreateEnvWrapperScript`): Generates a per-platform wrapper (`%TEMP%/UnrealClaude/claude-wrapper.cmd` on Windows, `/tmp/UnrealClaude/claude-wrapper.sh` on Linux/Mac) that sets the env vars and then forwards all arguments to the real `claude` binary (`%*` / `"$@"`). The wrapper is cached in `CachedWrapperPath` and reused across requests.
- **Async execution** (`FClaudeCodeRunner::LaunchProcess`): Spawns the wrapper script directly through `FPlatformProcess::CreateProc`, so the child process inherits the injected env vars. Stdin/stdout pipes still work because the wrapper transparently forwards I/O.
- **Sync execution** (`FClaudeCodeRunner::ExecuteSync`): Uses the same wrapper script via `FPlatformProcess::ExecProcess`, avoiding inline shell escaping issues.
- **Cleanup**: The wrapper script is deleted in the `~FClaudeCodeRunner` destructor. If the editor crashes, the OS temp directory janitor will eventually clean it up.

If you want to override these defaults, you can still set the variables in your shell before launching the editor — the plugin's hardcoded values will take precedence for the child process. To change the injected values, edit `CreateEnvWrapperScript()` in `ClaudeCodeRunner.cpp`.

## Important File Locations

- `UnrealClaude/Source/UnrealClaude/` — C++ plugin source
- `UnrealClaude/Source/UnrealClaude/Public/` — Public headers (API surface)
- `UnrealClaude/Source/UnrealClaude/Private/MCP/Tools/` — MCP tool implementations
- `UnrealClaude/Source/UnrealClaude/Private/Tests/` — C++ automation tests
- `UnrealClaude/Resources/mcp-bridge/` — Node.js MCP bridge (git submodule)
- `UnrealClaude/Resources/mcp-bridge/contexts/` — Dynamic UE 5.7 context markdown files
- `UnrealClaude/CLAUDE.md.default` — Template for per-project custom context
- `build.bat` — Reference Windows build command (author's local paths, not portable)
