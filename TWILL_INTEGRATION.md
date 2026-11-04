# Twill 内嵌调试库适配

本 fork 从 G-Yong/QuickJS-Debugger 的 `552efcf9f48396a646d23e3fda01b05e6c7469d3` 派生；保留原 MIT 许可和版权。它不是旧本地 `d097bc5…` 提交的恢复：旧适配源码在本次工作树中不可用，本次针对已有 Twill 契约重新适配并单独验证。

## 消费边界

Twill 自己拥有 JSRuntime/JSContext、Gameplay ESM 加载、固定 Tick、Runtime/Player 控制协议和停止/释放顺序。Twill **不调用本仓库顶层 CMake**，不编译或启动 `qjs_debug` / `debugger/src/main.cpp`，也不初始化或链接本仓库嵌套的 `quickjs/`。上游示例入口保留作来源参考，不属于本次受支持的 Twill 集成路径。

主仓库 `cmake/TwillScriptDebugger.cmake` 从 `debugger/src/cdp/` 显式编译：

- `debug_session.cpp`、`debug_session_values.cpp`：断点、源码单步、暂停、受限数据检查和基本值修改。
- `cdp_handler.cpp`：CDP 请求/响应与 VS Code 数据型 helper 适配。
- `websocket_server.cpp`：loopback WebSocket/HTTP discovery 传输。
- 同目录头文件（含原内嵌 `json.h`）作为库内部依赖。

这些文件组成静态库，链接到主仓库固定的 `third_party/quickjs-ng-misaka`，不另建 VM 或进程。宿主安装 trace/exception handler 后再编译脚本，登记真实源码/inline Source Map；先停止 session/socket、解除等待并 join transport，再移除 handler 和释放 Context。

## 策略与验证

Twill 支持集的权威定义在主仓库 `Docs/implementation/15_QuickJS_Debugging_Support.md` §9.1/§10：

- transport 只监听 `127.0.0.1`，不能切换为 LAN listener；
- 调试请求通过有界队列在安全暂停的 VM 线程执行；transport 线程不访问 VM；
- 自动检查不执行 getter、Proxy trap、方法或任意 JavaScript；
- 只读表达式仅支持绑定与普通数据路径；直接改值只接受基本值、准确定位的可写绑定或已有可写数据字段；
- CDP `callFunctionOn` 仅解析白名单数据赋值 helper，不编译或执行 helper；任意求值不是此集成的能力；
- 暂停时间回报宿主并从模拟 elapsed 扣除，断连解除等待、恢复执行并使旧 handle 失效；
- 输入/输出、JSON 深度/节点、帧/对象/属性、字符串和路径有明确预算。

本次真实验证入口由主仓库提供：`twill_script_debug_vendor_check`（tests-ON 的显式库级检查）、`twill_script_debug_authoring_check`、`twill_script_debug_cli_check`、`twill_native_script_debugger_check`。构建/测试结果与未覆盖项记录在主仓库实施记录的新接入章节；不沿用旧适配版的 GUI 或运行证据，也不宣称上游 standalone CLI 已重新适配。

适配提交只获授权在本地创建，推送/发布由仓库所有者单独决定。主仓库应锁定实际适配 commit；未发布的本地提交不能冒充其他机器可获取的共享交付。
