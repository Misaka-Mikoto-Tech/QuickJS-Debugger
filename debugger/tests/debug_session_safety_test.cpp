#include "../src/cdp/debug_session.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Vm {
    JSRuntime* runtime = JS_NewRuntime();
    JSContext* context = runtime ? JS_NewContext(runtime) : nullptr;
    Vm() { require(context != nullptr, "VM setup failed"); }
    ~Vm() { if (context) JS_FreeContext(context); if (runtime) JS_FreeRuntime(runtime); }
};
void install(JSContext* ctx, DebugSession& session) {
    JS_SetDebugTraceHandler(ctx, DebugSession::debug_trace_handler, &session);
    session.install_exception_handler(ctx); session.set_enabled(true);
}
void uninstall(JSContext* ctx, DebugSession& session) {
    session.stop(); session.uninstall_exception_handler(ctx); JS_SetDebugTraceHandler(ctx, nullptr, nullptr);
}
bool evaluate(JSContext* ctx, const char* code) {
    JSValue value = JS_Eval(ctx, code, std::strlen(code), "safety.js", JS_EVAL_TYPE_GLOBAL);
    const bool ok = !JS_IsException(value); JS_FreeValue(ctx, value);
    if (!ok) JS_FreeValue(ctx, JS_GetException(ctx));
    return ok;
}
void shadow_and_stale() {
    Vm vm;
    DebugSession session; install(vm.context, session);
    std::mutex mutex; std::condition_variable cv; json::Value pause; size_t count = 0;
    session.set_send_event([&](const std::string& event, const json::Value& params) {
        if (event != "Debugger.paused") return;
        std::lock_guard<std::mutex> guard(mutex); pause = params; ++count; cv.notify_all();
    });
    std::exception_ptr problem;
    std::thread transport([&] {
        try {
            auto next_pause = [&](size_t expected) {
                std::unique_lock<std::mutex> guard(mutex);
                require(cv.wait_for(guard, std::chrono::seconds(5), [&] { return count >= expected; }), "Pause watchdog");
                return pause;
            };
            const auto first = next_pause(1);
            const auto frame = first["callFrames"][0];
            const auto frame_id = frame["callFrameId"].get_string();
            int outer = -1, inner = -1; std::string object_id;
            const auto& chain = frame["scopeChain"].get_array();
            for (size_t i = 0; i < chain.size(); ++i) {
                const auto properties = session.get_properties(chain[i]["object"]["objectId"].get_string());
                require(!properties.has("error"), "Scope inspection refused");
                for (const auto& property : properties["result"].get_array()) {
                    const auto name = property["name"].get_string();
                    if (name == "value" && property["value"]["value"].get_number() == 1) outer = static_cast<int>(i);
                    if (name == "value" && property["value"]["value"].get_number() == 2) inner = static_cast<int>(i);
                    if (name == "data") object_id = property["value"]["objectId"].get_string();
                }
            }
            require(outer >= 0 && inner >= 0 && outer != inner, "Lexical scope identities were collapsed");
            require(!object_id.empty(), "Missing lazy object handle");
            require(session.evaluate_on_call_frame(frame_id, "value").has("error"), "Ambiguous textual path must not guess");
            auto replacement = json::Value::object(); replacement.set("value", 9);
            require(!session.set_variable_value(inner, "value", replacement, frame_id).has("error"), "Inner precise-slot write failed");
            replacement.set("value", 4);
            require(!session.set_variable_value(outer, "value", replacement, frame_id).has("error"), "Outer precise-slot write failed");
            session.resume();
            const auto second = next_pause(2);
            require(second["callFrames"][0]["callFrameId"].get_string() != frame_id, "Pause generation not advanced");
            require(session.evaluate_on_call_frame(frame_id, "data").has("error"), "Stale frame accepted");
            require(session.get_properties(object_id).has("error"), "Stale object accepted");
            session.resume();
        } catch (...) { problem = std::current_exception(); session.stop(); }
    });
    const bool ok = evaluate(vm.context,
        "function run(){ let value=1; const data={x:1}; let selected=0;"
        "{let value=2; debugger; selected=value;} debugger; return selected*10+value; }"
        "globalThis.answer=run();");
    transport.join();
    uninstall(vm.context, session);
    if (problem) std::rethrow_exception(problem);
    require(ok, "Shadow fixture failed");
    JSValue global = JS_GetGlobalObject(vm.context), answer = JS_GetPropertyStr(vm.context, global, "answer");
    int32_t number = 0; JS_ToInt32(vm.context, &number, answer);
    JS_FreeValue(vm.context, answer); JS_FreeValue(vm.context, global);
    require(number == 94, "Scope writes touched the wrong binding");
}
void throwing_send() {
    Vm vm;
    DebugSession session; install(vm.context, session);
    std::mutex mutex; std::condition_variable cv; bool entered = false, request_started = false;
    std::string scope; json::Value response;
    std::thread transport([&] {
        {
            std::unique_lock<std::mutex> guard(mutex);
            if (!cv.wait_for(guard, std::chrono::seconds(5), [&] { return entered; })) { session.stop(); return; }
            request_started = true; cv.notify_all();
        }
        response = session.get_properties(scope);
    });
    session.set_send_event([&](const std::string& event, const json::Value& params) {
        if (event != "Debugger.paused") return;
        std::unique_lock<std::mutex> guard(mutex);
        scope = params["callFrames"][0]["scopeChain"][0]["object"]["objectId"].get_string();
        entered = true; cv.notify_all();
        require(cv.wait_for(guard, std::chrono::seconds(5), [&] { return request_started; }), "Requester watchdog");
        throw std::runtime_error("Injected sender failure");
    });
    const bool ok = evaluate(vm.context, "function run(){const data={x:1};debugger;return 42;}globalThis.answer=run();");
    transport.join();
    require(ok && response.has("error") && !session.is_paused() && !session.is_enabled(), "Throwing sender left a live pause/request");
    uninstall(vm.context, session);
    // VM destruction also checks that paused values/objects were released.
}
void throwing_observer() {
    Vm vm;
    DebugSession session; install(vm.context, session);
    session.set_send_event([&](const std::string& event, const json::Value&) { if (event == "Debugger.paused") session.resume(); });
    session.set_pause_observer([](std::chrono::steady_clock::duration) { throw std::runtime_error("Injected observer failure"); });
    const bool ok = evaluate(vm.context, "function run(){const data={x:1};debugger;return 42;}run();");
    require(ok && !session.is_paused() && !session.is_enabled(), "Throwing observer left a live pause");
    uninstall(vm.context, session);
}
}

int main() {
    try {
        shadow_and_stale(); throwing_send(); throwing_observer();
        std::puts("PASS: explicit lexical slots, ambiguous-path refusal, stale handles, throwing callbacks cleanup");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
