#pragma once

#include "json.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
extern "C" {
#include "quickjs.h"
}

// Source metadata is registered by the embedding host before transport starts.
struct ScriptInfo {
    std::string id, url, source, source_map_url, compile_filename;
    int end_line = 0;
    bool is_module = false;
};
struct Breakpoint {
    int id = 0;
    std::string url, script_id;
    int line = 0, column = 0;
};
struct DebugBinding {
    std::string name;
    JSValue value = JS_UNDEFINED;
    int slot = -1;
    bool writable = false, initialized = false;
};
struct DebugScope {
    std::string id, type;
    int frame = 0, lexical_level = 0;
    std::vector<DebugBinding> bindings;
};
struct DebugFrame {
    std::string id;
    std::vector<size_t> scopes;
};
struct DebugObject {
    JSValue value = JS_UNDEFINED;
};
struct DebugRequest {
    std::function<json::Value(JSContext*)> operation;
    json::Value result;
    uint64_t generation = 0;
    bool done = false;
};

enum class PauseReason { Breakpoint, Step, PauseRequest, Entry, DebuggerStatement, Exception };

class DebugSession {
public:
    using SendEventFn = std::function<void(const std::string&, const json::Value&)>;
    using PauseObserver = std::function<void(std::chrono::steady_clock::duration)>;
    DebugSession();
    ~DebugSession();
    void set_send_event(SendEventFn fn);
    void set_pause_observer(PauseObserver observer);
    std::string add_script(const std::string& url, const std::string& source,
        bool is_module = false, const std::string& source_map_url = {},
        const std::string& compile_filename = {});
    const ScriptInfo* get_script_by_id(const std::string& id) const;
    const std::vector<ScriptInfo>& scripts() const { return scripts_; }
    json::Value script_parsed(const ScriptInfo& script) const;
    json::Value set_breakpoint_by_url(const std::string& url, int line, int column, bool is_regex = false);
    bool remove_breakpoint(const std::string& id);
    void set_breakpoints_active(bool active);
    void resume();
    void step_over();
    void step_into();
    void step_out();
    void request_pause();
    bool is_paused() const { return paused_.load(); }
    bool is_enabled() const { return enabled_.load(); }
    void set_enabled(bool enabled);
    json::Value get_call_frames_json() const;
    json::Value get_properties(const std::string& id);
    json::Value evaluate_on_call_frame(const std::string& frame, const std::string& expression);
    json::Value evaluate_literal(const std::string& expression);
    json::Value set_variable_value(int scope, const std::string& name, const json::Value& value, const std::string& frame);
    json::Value set_object_value(const std::string& object, const std::string& field, const json::Value& value);
    json::Value set_pause_on_exceptions(const std::string& state);
    void install_exception_handler(JSContext* ctx);
    void uninstall_exception_handler(JSContext* ctx);
    void stop();
    static int debug_trace_handler(JSContext*, JSAtom, JSAtom, int, int, int, void*);
    void set_pause_on_start(bool enabled);
    void wait_for_debugger();
    void run_if_waiting();
    void on_disconnect();
    static std::string normalize_url(const std::string&);
    static std::string to_file_url(const std::string&);
    static json::Value error(const std::string&);
    // Parse basic literals only; never evaluates source code.
    static bool parse_literal(const std::string&, json::Value& argument);

private:
    enum class StepMode { Continue, Into, Over, Out };
    void continue_with(StepMode mode);
    void do_pause(JSContext*, const std::string&, const std::string&, int, int,
        PauseReason, int breakpoint = 0, JSValueConst exception = JS_UNDEFINED);
    void capture_frames(JSContext*, const std::string&, const std::string&, int, int);
    void clear_pause(JSContext*);
    void refresh_bindings(JSContext*);
    json::Value dispatch(std::function<json::Value(JSContext*)> operation);
    json::Value remote(JSContext*, JSValueConst);
    bool ordinary(JSContext*, JSValueConst) const;
    bool primitive(JSContext*, const json::Value&, JSValue&);
    bool read_own(JSContext*, JSValueConst, const std::string&, JSValue&);
    json::Value properties_vm(JSContext*, const std::string&);
    json::Value evaluate_vm(JSContext*, const std::string&, const std::string&);
    int find_frame(const std::string&) const;
    std::string find_script_id(const std::string&) const;
    static void exception_handler(JSContext*, JSValueConst, bool, void*);

    SendEventFn send_event_;
    PauseObserver pause_observer_;
    std::vector<ScriptInfo> scripts_;
    std::map<int, Breakpoint> breakpoints_;
    mutable std::mutex mutex_;
    std::condition_variable pause_cv_, request_cv_, start_cv_;
    std::atomic<bool> enabled_{false}, paused_{false}, stopping_{false};
    bool breakpoints_active_ = true, pause_on_start_ = false, entry_pending_ = false;
    bool run_received_ = false, disconnected_ = false, pause_requested_ = false;
    StepMode step_mode_ = StepMode::Continue;
    int step_depth_ = 0, step_line_ = 0, current_depth_ = 0, current_line_ = 0;
    std::string step_file_, current_file_, exception_policy_ = "none";
    int next_breakpoint_ = 1;
    uint64_t generation_ = 0;
    DebugRequest* pending_ = nullptr;
    // Everything below is accessed only on the VM thread while mutex_ is held.
    std::vector<DebugFrame> frames_;
    std::vector<DebugScope> scopes_;
    std::map<std::string, DebugObject> objects_;
    std::map<void*, std::string> object_ids_;
    json::Value captured_frames_;
    size_t next_object_ = 1, preview_bytes_ = 0, handle_count_ = 0;
    JSClassID object_class_ = 0, array_class_ = 0;
};
