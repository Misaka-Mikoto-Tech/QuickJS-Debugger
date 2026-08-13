[English](./README.md) | **中文**

# QuickJS CDP Debugger

基于 Chrome DevTools Protocol (CDP) 的 QuickJS 脚本调试器，支持在 VS Code 中对自定义 QuickJS 引擎进行断点调试。

![Debug in VSCode](doc/images/debugInVSCode.png)

## 调试器特性

- **断点** — 支持基于 URL 或 URL 正则的行断点，可启用 / 禁用。
- **单步** — step over / into / out，以及 pause / resume。
- **`debugger;` 语句** — 用户代码中可达的 `debugger;` 语句无需断点即触发暂停。
- **调用栈检视** — 完整的 JS 调用栈，含文件 / 函数 / 行号。
- **作用域链** — 每帧除 **Local**（参数 + 局部变量）外，还会暴露一个
  **Closure** 作用域（从外层函数捕获的闭包变量）；其值通过 `var_refs`
  实时读取，会反映内层函数对变量的修改。
- **`Debugger.setVariableValue`** — 在暂停状态下修改任意栈帧上的参数 /
  局部 / 闭包变量；`const` 绑定与 TDZ 状态会返回明确错误。
- **`Debugger.evaluateOnCallFrame`** — 在某个暂停栈帧的**词法环境**下求
  值表达式，可直接访问该帧的参数、局部、闭包变量以及全局；底层使用
  `JS_EvalInStackFrame()`。
- **`Debugger.scriptParsed` / `scriptFailedToParse`** — 脚本加载与解析失败
  （SyntaxError）时分别推送。

## 支持的 CDP 命令

| Domain    | 方法                              |
|-----------|----------------------------------|
| Debugger  | `enable` / `disable`             |
| Debugger  | `setBreakpointByUrl`             |
| Debugger  | `removeBreakpoint`               |
| Debugger  | `setBreakpointsActive`           |
| Debugger  | `getScriptSource`                |
| Debugger  | `resume`                         |
| Debugger  | `stepOver` / `stepInto` / `stepOut` |
| Debugger  | `pause`                          |
| Debugger  | `evaluateOnCallFrame`            |
| Debugger  | `setVariableValue`               |
| Debugger  | `setPauseOnExceptions`           |
| Runtime   | `enable`                         |
| Runtime   | `runIfWaitingForDebugger`        |
| Runtime   | `getProperties`                  |
| Runtime   | `evaluate`                       |
| Profiler  | `enable` / `disable`             |

发出的 CDP 事件：`Debugger.scriptParsed`、`Debugger.scriptFailedToParse`、
`Debugger.paused`、`Debugger.resumed`、`Runtime.consoleAPICalled`、
`Runtime.executionContextCreated`。

## 运行

### 基本运行

```bash
# 启动调试器，等待 VS Code 连接后再执行脚本
./build/debugger/qjs_debug --inspect-brk test.js

# 启动调试器，脚本直接开始执行（仅断点生效）
./build/debugger/qjs_debug --inspect test.js
```

启动后会输出：
```
Debugger listening on ws://127.0.0.1:9229/debug
Waiting for debugger to connect...
```

### 在 VS Code 中调试

1. 启动 `qjs_debug`：
   ```bash
   ./build/debugger/qjs_debug --inspect-brk test.js
   ```

2. 在 VS Code 中打开工作区，确保 `.vscode/launch.json` 包含：
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

3. 在 `test.js` 中设置断点

4. 按 `F5` 选择 "Attach to QuickJS" 启动调试

5. VS Code 连接后脚本开始执行，命中断点时自动暂停，可查看变量、调用栈，支持单步等操作

### 命令行参数

| 参数             | 说明                                      |
|-----------------|-------------------------------------------|
| `--inspect`      | 启动调试服务，脚本立即执行                    |
| `--inspect-brk`  | 启动调试服务，等待调试器连接并发送 resume 后才执行 |
| `--port <N>`     | 指定 WebSocket 监听端口（默认 9229）          |

### Chrome DevTools 调试

除 VS Code 外，也可用 Chrome 调试：

1. 启动 `qjs_debug --inspect-brk test.js`
2. 打开 Chrome 浏览器，访问 `chrome://inspect`
3. 在 "Remote Target" 中找到并点击 "inspect"

## 编译

### 环境要求

| 平台    | 编译器                         | 其它        |
|---------|-------------------------------|------------|
| Windows | MSVC (VS2019+) 或 MinGW-w64   | CMake ≥ 3.10 |
| Linux   | GCC 或 Clang                   | CMake ≥ 3.10 |

### Windows (MSVC)

```powershell
# 1. 打开 VS 开发者命令行（或手动初始化环境）
& "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

# 2. 配置 + 编译
mkdir build
cd build
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
nmake
```

编译产物在 `build/debugger/qjs_debug.exe`。

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

### 关键构建选项说明

- **MSVC < 17.5**：自动注入 `compat/msvc/stdatomic.h` 垫片解决缺失 C11 `<stdatomic.h>` 的问题
- **无需编译时宏**：调试接口使用专用的 `OP_debug` 操作码，始终在语句边界发射。未设置处理程序时运行时开销几乎为零（仅一次 `unlikely` 分支判断）。可随时通过 `JS_SetDebugTraceHandler()` 挂载处理程序。`DIRECT_DISPATCH`（computed goto）保持完全启用。
- **`JSDebugTraceFunc` 回调签名**：`filename` 和 `funcname` 现在以 `JSAtom` 形式传入（而非 `const char*`）。在回调内部通过 `JS_AtomToCString()` / `JS_FreeCString()` 按需转换为字符串。若仅需与已知断点位置比较，可直接对比 atom 值，无需堆分配。

## 架构概览

```
  VS Code (DevTools 前端)
       │
       │  WebSocket (ws://127.0.0.1:9229/debug)
       ▼
  ┌─────────────────┐
  │ WebSocketServer │  接受连接、WebSocket 握手、帧编解码
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │   CDPHandler    │  解析 CDP JSON 消息，分发到对应 domain 处理
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │  DebugSession   │  调试核心逻辑
  │  ─ 断点管理      │  set/remove breakpoint
  │  ─ 单步控制      │  step over / into / out / continue
  │  ─ 暂停/恢复     │  pause / resume
  │  ─ 调用栈捕获    │  capture call frames + scope variables
  │  ─ debug_trace   │  OP_debug 回调（检查断点/单步）
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │   QuickJS 引擎   │  执行 JS 脚本，通过调试回调触发
  │  ─ JS_SetDebugTraceHandler()     设置/清除调试跟踪回调
  │  ─ JSDebugTraceFunc(ctx, JSAtom filename, JSAtom funcname, line, col, flags, opaque)
  │  ─ OP_debug opcode               语句边界触发回调
  │  ─ JS_GetStackDepth()            获取调用栈深度
  │  ─ JS_GetLocalVariablesAtLevel() 获取指定层级局部变量
  │  ─ JS_FreeLocalVariables()       释放变量数组
  └─────────────────┘
```

### 调试流程

1. `qjs_debug` 启动 → 加载脚本 → 在端口 9229 启动 WebSocket 服务，等待调试器连接
2. VS Code 通过 `launch.json` 的 attach 配置连接到 `ws://127.0.0.1:9229/debug`
3. VS Code 发送 `Debugger.enable`、`Debugger.setBreakpointByUrl` 等 CDP 命令
4. `CDPHandler` 解析消息并调用 `DebugSession` 设置断点
5. `Runtime.runIfWaitingForDebugger` 解除阻塞，开始执行脚本
6. QuickJS 在语句边界触发 `OP_debug` 操作码 → 调用调试回调 → 检查断点/单步条件
7. 命中断点时 `do_pause()` 捕获调用栈和变量，发送 `Debugger.paused` 事件，阻塞等待
8. VS Code 收到暂停事件后显示断点位置和变量；用户操作 continue/step 发送对应 CDP 命令
9. `DebugSession` 收到命令后解除阻塞，脚本继续执行

## QuickJS 调试 API

本项目依赖的 QuickJS 扩展 API（在 `quickjs.h` 中声明）：

所有调试 API 始终可用，无需编译时宏控制。

### 调试跟踪回调签名

```c
/* 传给回调 `flags` 参数的标志位 */
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

`filename` 与 `funcname` 为 `JSAtom` 值，仅在回调期间有效。需要字符串时通过 `JS_AtomToCString()` 转换，用完后调用 `JS_FreeCString()` 释放。匿名函数或 eval 代码时，任一字段可为 `JS_ATOM_NULL`（值为 0）。

`flags` 携带本次回调的额外语义信息。目前仅定义一个位：`JS_DEBUG_TRACE_DEBUGGER_STMT` 仅在回调由 `debugger;` 语句（编译为专用的 `OP_debugger_stmt` 操作码）触发时置位，使嵌入者无需扫描源码即可区分真正的 `debugger;` 语句与语句边界的 `OP_debug`。

### 栈帧变量检视

```c
typedef struct {
    const char *name;      // 变量名（UTF-8，由数组拥有）
    JSValue value;         // 借用值；如需保留请 dup
    bool is_arg;           // 是否为函数实参
    bool is_closure;       // 是否为外层捕获的闭包变量
    int scope_level;       // 词法作用域深度（0 = 函数顶层）
} JSDebugLocalVar;

int  JS_GetStackDepth(JSContext *ctx);
int  JS_GetLocalVariablesAtLevel(JSContext *ctx, int level,
                                 JSDebugLocalVar **vars_out,
                                 int *count_out);
void JS_FreeLocalVariables(JSContext *ctx,
                           JSDebugLocalVar *vars, int count);
```

返回数组依次为：参数、局部变量、**外层捕获的闭包变量**
（`is_closure == true`）。处于 TDZ 或名字以 `<` 开头的内部变量会被跳过。

### 修改栈帧上的变量

```c
// 返回值：
//    0 成功（取得 new_value 所有权），
//   -1 帧上找不到该变量名，
//   -2 该绑定为 `const`，
//   -3 内部错误。
int JS_SetVariableAtLevel(JSContext *ctx, int level,
                          const char *name, JSValue new_value);
```

按"参数 → 局部 → 闭包"的顺序按名查找；用于实现 CDP 的
`Debugger.setVariableValue`。

### 在栈帧中求值表达式

```c
JSValue JS_EvalInStackFrame(JSContext *ctx, int level,
                            const char *input, size_t input_len,
                            const char *filename);
```

把 `input` 作为表达式在 `level` 处栈帧的词法环境下求值。实现上临时将运行时的当前栈帧指向目标帧，复用引擎原有的直接 eval 管道（`JS_EVAL_TYPE_DIRECT`），因此标识符解析走与运行时 `eval(...)` 运算符完全相同的路径——可访问参数、`var` 局部变量、块作用域 `let` / `const` 以及闭包变量，还包括全局。返回结果值，或在解析 / 运行出错时返回 `JS_EXCEPTION`。用于实现 CDP 的 `Debugger.evaluateOnCallFrame`。

### `debugger` 语句

对 `debugger;` 语句，解析器发射专用的 `OP_debugger_stmt` 操作码（始终发射，与当前是否已挂接处理程序无关）。后挂接的 `JSDebugTraceFunc` 在这些位置会看到 `flags & JS_DEBUG_TRACE_DEBUGGER_STMT` 被置位，从而无需扫描源码即可区分真正的 `debugger;` 语句与语句边界的 `OP_debug`。CDP 调试器以 `reason: "debugCommand"` 暂停。

## 注意事项

- `quickjs/` 目录包含 [quickjs-ng/quickjs](https://github.com/quickjs-ng/quickjs) 的修改版本，增加了调试接口（[PR #1421](https://github.com/quickjs-ng/quickjs/pull/1421)）。`OP_debug` 操作码始终发射，未设置处理程序时运行时开销几乎为零。
- Windows 上路径大小写不敏感，调试器内部已做统一处理
- `--inspect-brk` 模式下程序会阻塞直到调试器连接并发送 `runIfWaitingForDebugger`
