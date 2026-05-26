**English** | [中文](./README.zh-CN.md)

# QuickJS CDP Debugger

A QuickJS script debugger based on the Chrome DevTools Protocol (CDP), enabling breakpoint debugging of a custom QuickJS engine in VS Code.

![Debug in VSCode](doc/images/debugInVSCode.png)

## Project Structure

```
jsTest/
├── CMakeLists.txt              # Top-level build file
├── README.md
├── test.js                     # Test script
├── quickjs/                    # QuickJS engine (modified, with debug API)
│   ├── quickjs.h/c             #   Core engine
│   └── ...
├── debugger/                   # CDP debugger
│   ├── CMakeLists.txt          #   Debugger build file
│   └── src/
│       ├── main.cpp            #   Entry: creates QuickJS runtime + starts debug service
│       ├── websocket_server.h/cpp  # WebSocket server (port 9229)
│       ├── cdp_handler.h/cpp   #   CDP message dispatch & handling
│       ├── debug_session.h/cpp #   Debug core: breakpoints, stepping, pause, variable capture
│       ├── json.h              #   Lightweight JSON parse/serialize
│       └── platform.h          #   Cross-platform socket abstraction
├── compat/
│   └── msvc/
│       └── stdatomic.h         # C11 stdatomic shim for MSVC < 17.5
└── .vscode/
    └── launch.json             # VS Code debug attach configuration
```

## Architecture Overview

```
  VS Code (DevTools Frontend)
       │
       │  WebSocket (ws://127.0.0.1:9229/debug)
       ▼
  ┌─────────────────┐
  │ WebSocketServer │  Accept connections, WebSocket handshake, frame encode/decode
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │   CDPHandler    │  Parse CDP JSON messages, dispatch to domain handlers
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │  DebugSession   │  Core debug logic
  │  - Breakpoints  │  set/remove breakpoint
  │  - Stepping     │  step over / into / out / continue
  │  - Pause/Resume │  pause / resume
  │  - Call Stack   │  capture call frames + scope variables
  │  - debug_trace  │  OP_debug callback (check breakpoint/step)
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │  QuickJS Engine │  Execute JS scripts, trigger debug callbacks
  │  - JS_SetDebugTraceHandler()      set/clear debug trace callback
  │  - JSDebugTraceFunc(ctx, JSAtom filename, JSAtom funcname, line, col, flags, opaque)
  │  - OP_debug opcode                fires callback at statement boundaries
  │  - JS_GetStackDepth()             get call stack depth
  │  - JS_GetLocalVariablesAtLevel()  get locals at a given frame level
  │  - JS_FreeLocalVariables()        free variable array
  └─────────────────┘
```

### Debug Flow

1. `qjs_debug` starts → loads script → starts WebSocket server on port 9229, waits for debugger connection
2. VS Code connects to `ws://127.0.0.1:9229/debug` via the attach configuration in `launch.json`
3. VS Code sends CDP commands such as `Debugger.enable` and `Debugger.setBreakpointByUrl`
4. `CDPHandler` parses messages and calls `DebugSession` to set breakpoints
5. `Runtime.runIfWaitingForDebugger` unblocks execution, script starts running
6. QuickJS hits `OP_debug` opcodes at statement boundaries → invokes the debug break callback → checks breakpoint/step conditions
7. When a breakpoint is hit, `do_pause()` captures the call stack and variables, sends a `Debugger.paused` event, and blocks
8. VS Code receives the paused event and displays the breakpoint location and variables; user actions (continue/step) send corresponding CDP commands
9. `DebugSession` receives the command and unblocks, script resumes execution

## Building

### Requirements

| Platform | Compiler                      | Other       |
|----------|-------------------------------|-------------|
| Windows  | MSVC (VS2019+) or MinGW-w64  | CMake ≥ 3.10 |
| Linux    | GCC or Clang                  | CMake ≥ 3.10 |

### Windows (MSVC)

Open a **Developer Command Prompt for VS** (or initialize the environment manually), then:

```cmd
mkdir build
cd build
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
nmake
```

The output binary is `build/debugger/qjs_debug.exe`.

### Windows (MinGW)

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
mingw32-make
```

### Linux

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Key Build Notes

- **MSVC < 17.5**: Automatically injects the `compat/msvc/stdatomic.h` shim to resolve the missing C11 `<stdatomic.h>`.
- **No compile-time flags required**: The debug interface uses a dedicated `OP_debug` opcode that is always emitted at statement boundaries. The runtime cost is virtually zero when no handler is set (a single `unlikely` branch). Attach a handler at any time via `JS_SetDebugTraceHandler()`. `DIRECT_DISPATCH` (computed goto) remains fully enabled.
- **`JSDebugTraceFunc` callback signature**: `filename` and `funcname` are passed as `JSAtom` values (not `const char*`). Use `JS_AtomToCString()` / `JS_FreeCString()` inside the callback to convert them to strings when needed. This avoids a heap allocation on every instrumented statement when you only need to compare against known breakpoint atoms.

## Usage

### Basic Usage

```bash
# Start debugger, wait for VS Code to connect before executing the script
./build/debugger/qjs_debug --inspect-brk test.js

# Start debugger, script runs immediately (only breakpoints are active)
./build/debugger/qjs_debug --inspect test.js
```

Output on startup:
```
Debugger listening on ws://127.0.0.1:9229/debug
Waiting for debugger to connect...
```

### Debugging in VS Code

1. Start `qjs_debug`:
   ```bash
   ./build/debugger/qjs_debug --inspect-brk test.js
   ```

2. Open the workspace in VS Code and ensure `.vscode/launch.json` contains:
   ```json
   {
       "configurations": [{
           "type": "node",
           "request": "attach",
           "name": "Attach to QuickJS",
           "address": "127.0.0.1",
           "port": 9229,
           "localRoot": "${workspaceFolder}",
           "remoteRoot": "${workspaceFolder}",
           "skipFiles": ["<node_internals>/**"]
       }]
   }
   ```

3. Set breakpoints in `test.js`

4. Press `F5` and select "Attach to QuickJS"

5. Once VS Code connects, the script begins execution. It pauses at breakpoints automatically, allowing you to inspect variables, view the call stack, and step through code.

### Command-Line Arguments

| Argument          | Description                                                        |
|-------------------|--------------------------------------------------------------------|
| `--inspect`       | Start debug service; script runs immediately                       |
| `--inspect-brk`   | Start debug service; wait for debugger to connect and resume       |
| `--port <N>`      | Specify WebSocket listen port (default: 9229)                      |

### Debugging with Chrome DevTools

In addition to VS Code, you can also debug with Chrome:

1. Start `qjs_debug --inspect-brk test.js`
2. Open Chrome and navigate to `chrome://inspect`
3. Find the target under "Remote Target" and click "inspect"

## Supported CDP Commands

| Domain    | Methods                           |
|-----------|-----------------------------------|
| Debugger  | `enable` / `disable`              |
| Debugger  | `setBreakpointByUrl`              |
| Debugger  | `removeBreakpoint`                |
| Debugger  | `setBreakpointsActive`            |
| Debugger  | `getScriptSource`                 |
| Debugger  | `resume`                          |
| Debugger  | `stepOver` / `stepInto` / `stepOut` |
| Debugger  | `pause`                           |
| Debugger  | `evaluateOnCallFrame`             |
| Debugger  | `setVariableValue`                |
| Debugger  | `setPauseOnExceptions`            |
| Runtime   | `enable`                          |
| Runtime   | `runIfWaitingForDebugger`         |
| Runtime   | `getProperties`                   |
| Runtime   | `evaluate`                        |
| Profiler  | `enable` / `disable`              |

CDP events emitted: `Debugger.scriptParsed`, `Debugger.scriptFailedToParse`,
`Debugger.paused`, `Debugger.resumed`, `Runtime.consoleAPICalled`,
`Runtime.executionContextCreated`.

## Debugger Features

- **Breakpoints** — line breakpoints by URL or by URL regex, with active/inactive toggle.
- **Stepping** — step over / into / out, plus pause / resume.
- **`debugger;` statement** — pauses execution at any reachable `debugger;`
  statement in user code, regardless of breakpoints.
- **Call stack inspection** — full JS call stack with file / function / line.
- **Scope chain** — each frame exposes a **Local** scope (arguments and
  locals) and a **Closure** scope (variables captured from enclosing
  functions).  Closure variable values are read live from the underlying
  `var_refs` and reflect mutations made through inner functions.
- **`Debugger.setVariableValue`** — write a new value to any argument /
  local / closure variable on any paused stack frame.  `const` bindings and
  TDZ slots are rejected with a clear error.
- **`Debugger.evaluateOnCallFrame`** — evaluates an expression in the
  *context of a paused stack frame*, with access to that frame's arguments,
  locals and closures (in addition to globals).  Powered by
  `JS_EvalInStackFrame()`.
- **`Debugger.scriptParsed` / `scriptFailedToParse`** — emitted when a script
  is loaded and when it fails to parse (SyntaxError).

## QuickJS Debug API

Extended QuickJS APIs this project depends on (declared in `quickjs.h`):

All debug APIs are always available — no compile-time flags required.

### Debug Trace Callback

```c
/* Flag bits passed to the callback's `flags` argument. */
#define JS_DEBUG_TRACE_DEBUGGER_STMT (1 << 0)

typedef int JSDebugTraceFunc(JSContext *ctx,
                             JSAtom filename,
                             JSAtom funcname,
                             int line,
                             int col,
                             int flags,
                             void *opaque);

void JS_SetDebugTraceHandler(JSContext *ctx,
                             JSDebugTraceFunc *func,
                             void *opaque);
```

`filename` and `funcname` are `JSAtom` values, valid only for the duration of
the callback. Convert with `JS_AtomToCString()` and free with
`JS_FreeCString()`. Either may be `JS_ATOM_NULL` for anonymous functions or
eval code.

`flags` carries extra information about why the callback fired. Currently
only one bit is defined: `JS_DEBUG_TRACE_DEBUGGER_STMT` is set when the
callback was triggered by a `debugger;` statement (compiled as the dedicated
`OP_debugger_stmt` opcode) as opposed to a normal statement-boundary `OP_debug`.

### Stack-frame Inspection

```c
typedef struct {
    const char *name;      // variable name (UTF-8, owned by the array)
    JSValue value;         // borrowed value; dup if you keep it
    bool is_arg;           // true if this is a function argument
    bool is_closure;       // true if captured from an enclosing scope
    int scope_level;       // lexical scope depth (0 = top of function)
} JSDebugLocalVar;

int  JS_GetStackDepth(JSContext *ctx);
int  JS_GetLocalVariablesAtLevel(JSContext *ctx, int level,
                                 JSDebugLocalVar **vars_out,
                                 int *count_out);
void JS_FreeLocalVariables(JSContext *ctx,
                           JSDebugLocalVar *vars, int count);
```

Returned array contains, in order: arguments, then locals, then **closure
variables** captured from enclosing functions (`is_closure == true`).
Variables in TDZ or whose source name is internal (starts with `<`) are
skipped.

### Mutating a Variable on a Stack Frame

```c
// Returns:
//    0 on success (takes ownership of new_value),
//   -1 if no variable with that name on the frame,
//   -2 if the binding is `const`,
//   -3 on internal error.
int JS_SetVariableAtLevel(JSContext *ctx, int level,
                          const char *name, JSValue new_value);
```

Searches arguments first, then locals, then closures, by name.  Used to
implement CDP's `Debugger.setVariableValue`.

### Expression Evaluation in a Stack Frame

```c
JSValue JS_EvalInStackFrame(JSContext *ctx, int level,
                            const char *input, size_t input_len,
                            const char *filename);
```

Evaluates `input` as an expression in the lexical environment of the stack
frame at `level`.  Implemented by temporarily setting the runtime's current
stack frame to the target frame and reusing the engine's direct-eval pipeline
(`JS_EVAL_TYPE_DIRECT`), so identifiers resolve through the same path the
`eval(...)` operator uses at runtime -- including arguments, var-declared
locals, block-scoped `let` / `const` bindings and closure variables in
addition to the global scope.  Returns the result value, or `JS_EXCEPTION`
on parse / runtime error.  Used to implement CDP's
`Debugger.evaluateOnCallFrame`.

### `debugger` Statement

The parser emits a dedicated `OP_debugger_stmt` opcode for `debugger;`
statements (always, independent of whether a handler is currently
attached).  Any `JSDebugTraceFunc` set later observes those locations with
`flags & JS_DEBUG_TRACE_DEBUGGER_STMT` set, so the embedder can distinguish
a real `debugger;` statement from a statement-boundary `OP_debug` without
any source-text inspection.  The CDP debugger pauses with `reason:
"debugCommand"`.

## Notes

- The `quickjs/` directory contains a modified fork of [quickjs-ng/quickjs](https://github.com/quickjs-ng/quickjs) with the debug interface additions ([PR #1421](https://github.com/quickjs-ng/quickjs/pull/1421)). `OP_debug` opcodes are always emitted; the runtime cost is virtually zero when no handler is set.
- On Windows, file paths are case-insensitive; the debugger handles normalization internally.
- In `--inspect-brk` mode, the program blocks until a debugger connects and sends `runIfWaitingForDebugger`.
