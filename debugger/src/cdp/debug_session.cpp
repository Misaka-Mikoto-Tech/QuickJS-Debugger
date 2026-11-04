#include "debug_session.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace {
std::string atom_text(JSContext* ctx, JSAtom atom) {
    const char* value = JS_AtomToCString(ctx, atom);
    std::string result = value ? value : "";
    if (value) JS_FreeCString(ctx, value);
    return result;
}
bool same_path(const std::string& a, const std::string& b) {
#ifdef _WIN32
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    return lower(a) == lower(b);
#else
    return a == b;
#endif
}
}

DebugSession::DebugSession() = default;
DebugSession::~DebugSession() = default;
void DebugSession::set_send_event(SendEventFn fn) { send_event_ = std::move(fn); }
void DebugSession::set_pause_observer(PauseObserver fn) { pause_observer_ = std::move(fn); }

std::string DebugSession::normalize_url(const std::string& path) {
    std::string value = path;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.rfind("file:///", 0) == 0 && value.size() > 10 && value[9] == ':') value.erase(0, 8);
    else if (value.rfind("file://", 0) == 0) value.erase(0, 7);
    return value;
}
std::string DebugSession::to_file_url(const std::string& path) {
    if (path.find("://") != std::string::npos) return path;
    auto norm = normalize_url(path);
    if (!norm.empty() && norm[0] == '/') return "file://" + norm;
    if (norm.size() > 2 && norm[1] == ':') return "file:///" + norm;
    return norm;
}
json::Value DebugSession::error(const std::string& message) {
    auto result = json::Value::object(), detail = json::Value::object();
    detail.set("code", -32000).set("message", message); result.set("error", detail); return result;
}
std::string DebugSession::add_script(const std::string& url, const std::string& source,
        bool module, const std::string& map, const std::string& compile) {
    ScriptInfo info;
    info.id = std::to_string(scripts_.size() + 1);
    info.url = to_file_url(url); info.source = source; info.is_module = module;
    info.source_map_url = map; info.compile_filename = compile.empty() ? normalize_url(url) : normalize_url(compile);
    info.end_line = 1 + static_cast<int>(std::count(source.begin(), source.end(), '\n'));
    scripts_.push_back(std::move(info));
    return scripts_.back().id;
}
const ScriptInfo* DebugSession::get_script_by_id(const std::string& id) const {
    for (const auto& source : scripts_) if (source.id == id) return &source;
    return nullptr;
}
json::Value DebugSession::script_parsed(const ScriptInfo& source) const {
    auto result = json::Value::object();
    result.set("scriptId", source.id).set("url", source.url).set("startLine", 0).set("startColumn", 0)
        .set("endLine", source.end_line).set("endColumn", 0).set("executionContextId", 1)
        .set("hash", "").set("isLiveEdit", false).set("sourceMapURL", source.source_map_url)
        .set("hasSourceURL", false).set("isModule", source.is_module).set("length", static_cast<int64_t>(source.source.size()));
    return result;
}
std::string DebugSession::find_script_id(const std::string& filename) const {
    const auto file = normalize_url(filename);
    for (const auto& source : scripts_) {
        if (same_path(file, source.compile_filename) || same_path(file, normalize_url(source.url))) return source.id;
    }
    return "0";
}
json::Value DebugSession::set_breakpoint_by_url(const std::string& url, int line, int column, bool regex) {
    if (regex || line < 0 || column < 0 || url.size() > 4096) return error("Unsupported breakpoint URL/regex/location");
    std::lock_guard<std::mutex> guard(mutex_);
    if (breakpoints_.size() >= 2048) return error("Breakpoint budget exceeded");
    Breakpoint bp; bp.id = next_breakpoint_++; bp.url = normalize_url(url); bp.line = line + 1; bp.column = column;
    for (const auto& source : scripts_) if (same_path(normalize_url(source.url), bp.url)) bp.script_id = source.id;
    breakpoints_[bp.id] = bp;
    auto locations = json::Value::array();
    if (!bp.script_id.empty()) {
        auto location = json::Value::object();
        location.set("scriptId", bp.script_id).set("lineNumber", line).set("columnNumber", column); locations.push(location);
    }
    auto result = json::Value::object();
    result.set("breakpointId", std::to_string(bp.id)).set("locations", locations); return result;
}
bool DebugSession::remove_breakpoint(const std::string& id) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
        if (std::to_string(it->first) == id) { breakpoints_.erase(it); return true; }
    }
    return false;
}
void DebugSession::set_breakpoints_active(bool value) { std::lock_guard<std::mutex> guard(mutex_); breakpoints_active_ = value; }
void DebugSession::set_enabled(bool value) {
    if (!value) { on_disconnect(); return; }
    std::lock_guard<std::mutex> guard(mutex_); disconnected_ = false; enabled_ = true;
}
void DebugSession::continue_with(StepMode mode) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!paused_) return;
    step_mode_ = mode; step_depth_ = current_depth_; step_line_ = current_line_; step_file_ = current_file_;
    pause_requested_ = false; paused_ = false;
    pause_cv_.notify_all();
}
void DebugSession::resume() { continue_with(StepMode::Continue); }
void DebugSession::step_over() { continue_with(StepMode::Over); }
void DebugSession::step_into() { continue_with(StepMode::Into); }
void DebugSession::step_out() { continue_with(StepMode::Out); }
void DebugSession::request_pause() { std::lock_guard<std::mutex> guard(mutex_); pause_requested_ = true; }
void DebugSession::set_pause_on_start(bool enabled) {
    std::lock_guard<std::mutex> guard(mutex_); pause_on_start_ = enabled; entry_pending_ = enabled;
}
void DebugSession::wait_for_debugger() {
    const auto start = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> guard(mutex_);
        if (!pause_on_start_) return;
        start_cv_.wait(guard, [this] { return run_received_ || stopping_ || disconnected_; });
    }
    if (pause_observer_) pause_observer_(std::chrono::steady_clock::now() - start);
}
void DebugSession::run_if_waiting() {
    std::lock_guard<std::mutex> guard(mutex_); run_received_ = true; start_cv_.notify_all();
}
void DebugSession::on_disconnect() {
    std::lock_guard<std::mutex> guard(mutex_);
    enabled_ = false; disconnected_ = true; paused_ = false; pause_requested_ = false; entry_pending_ = false;
    step_mode_ = StepMode::Continue;
    if (pending_) { pending_->result = error("Debugger detached"); pending_->done = true; pending_ = nullptr; }
    pause_cv_.notify_all(); request_cv_.notify_all(); start_cv_.notify_all();
}
void DebugSession::stop() { stopping_ = true; on_disconnect(); }
void DebugSession::install_exception_handler(JSContext* ctx) { JS_SetDebugExceptionHandler(ctx, exception_handler, this); }
void DebugSession::uninstall_exception_handler(JSContext* ctx) { JS_SetDebugExceptionHandler(ctx, nullptr, nullptr); }
json::Value DebugSession::set_pause_on_exceptions(const std::string& policy) {
    if (policy != "none" && policy != "all" && policy != "uncaught") return error("Unsupported exception policy");
    std::lock_guard<std::mutex> guard(mutex_); exception_policy_ = policy; return json::Value::object();
}
json::Value DebugSession::dispatch(std::function<json::Value(JSContext*)> operation) {
    std::unique_lock<std::mutex> guard(mutex_);
    if (!paused_ || stopping_ || !enabled_) return error("No active paused invocation");
    if (pending_) return error("Debug inspection queue budget exceeded");
    DebugRequest request; request.operation = std::move(operation); request.generation = generation_;
    pending_ = &request; pause_cv_.notify_all();
    request_cv_.wait(guard, [&] { return request.done; });
    return request.result;
}
json::Value DebugSession::get_call_frames_json() const {
    std::lock_guard<std::mutex> guard(mutex_); return paused_ ? captured_frames_ : json::Value::array();
}
void DebugSession::clear_pause(JSContext* ctx) {
    for (auto& scope : scopes_) for (auto& binding : scope.bindings) JS_FreeValue(ctx, binding.value);
    for (auto& object : objects_) JS_FreeValue(ctx, object.second.value);
    scopes_.clear(); frames_.clear(); objects_.clear(); object_ids_.clear(); captured_frames_ = json::Value::array();
    handle_count_ = 0; next_object_ = 1; preview_bytes_ = 0;
}
void DebugSession::capture_frames(JSContext* ctx, const std::string& filename, const std::string& function, int line, int column) {
    clear_pause(ctx); ++generation_;
    JSValue plain = JS_NewObject(ctx), array = JS_NewArray(ctx);
    if (JS_IsException(plain) || JS_IsException(array)) {
        JS_FreeValue(ctx, plain); JS_FreeValue(ctx, array);
        throw std::runtime_error("Debug object classification allocation failed");
    }
    object_class_ = JS_GetClassID(plain); array_class_ = JS_GetClassID(array);
    JS_FreeValue(ctx, plain); JS_FreeValue(ctx, array);
    captured_frames_ = json::Value::array();
    const int depth = std::min(JS_GetStackDepth(ctx), 64);
    for (int level = 0; level < depth; ++level) {
        JSAtom file = JS_ATOM_NULL, name = JS_ATOM_NULL; int frame_line = 0, frame_col = 0;
        if (JS_GetDebugFrame(ctx, level, &file, &name, &frame_line, &frame_col) < 0) continue;
        std::string frame_file = atom_text(ctx, file), frame_name = atom_text(ctx, name);
        if (level == 0) { frame_file = filename; frame_name = function; frame_line = line; frame_col = column; }
        DebugFrame frame; frame.id = std::to_string(generation_) + ":" + std::to_string(level);
        auto chain = json::Value::array();
        JSDebugLocalVar* vars = nullptr; int count = 0;
        if (JS_GetLocalVariablesAtLevel(ctx, level, &vars, &count) < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); count = 0; }
        // Preserve lexical scope identity; innermost lexical scopes precede outer scopes.
        std::map<std::pair<bool, int>, std::vector<int>> groups;
        for (int i = 0; i < count && i < 2048; ++i) groups[{vars[i].is_closure, -vars[i].scope_level}].push_back(i);
        for (const auto& group : groups) {
            if (handle_count_ >= 2048) break;
            DebugScope scope; scope.frame = level; scope.type = group.first.first ? "closure" : "local";
            scope.lexical_level = -group.first.second;
            scope.id = "scope:" + std::to_string(generation_) + ":" + std::to_string(scopes_.size());
            ++handle_count_;
            for (int i : group.second) {
                if (handle_count_ >= 2048) break;
                const auto& var = vars[i];
                DebugBinding binding; binding.name = var.name ? var.name : ""; binding.value = JS_DupValue(ctx, var.value);
                binding.slot = var.binding_index; binding.writable = var.is_writable; binding.initialized = var.is_initialized;
                scope.bindings.push_back(std::move(binding)); ++handle_count_;
            }
            auto item = json::Value::object(), object = json::Value::object();
            object.set("type", "object").set("className", "Object").set("objectId", scope.id);
            item.set("type", scope.type).set("object", object).set("name", frame_name);
            chain.push(item); frame.scopes.push_back(scopes_.size()); scopes_.push_back(std::move(scope));
        }
        if (vars) JS_FreeLocalVariables(ctx, vars, count);
        auto result = json::Value::object(), location = json::Value::object(), receiver = json::Value::object();
        const auto script_id = find_script_id(frame_file);
        const auto* source = get_script_by_id(script_id);
        location.set("scriptId", script_id).set("lineNumber", std::max(0, frame_line - 1)).set("columnNumber", std::max(0, frame_col - 1));
        receiver.set("type", "undefined");
        result.set("callFrameId", frame.id).set("functionName", frame_name).set("location", location)
            .set("url", source ? source->url : to_file_url(frame_file)).set("scopeChain", chain).set("this", receiver);
        captured_frames_.push(result); frames_.push_back(std::move(frame));
    }
}
void DebugSession::do_pause(JSContext* ctx, const std::string& file, const std::string& name,
        int line, int column, PauseReason reason, int breakpoint, JSValueConst exception) {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> guard(mutex_);
    if (!enabled_ || stopping_) return;
    capture_frames(ctx, file, name, line, column);
    current_depth_ = JS_GetStackDepth(ctx); current_line_ = line; current_file_ = file; paused_ = true;
    auto params = json::Value::object(), hits = json::Value::array();
    const char* reasons[] = {"breakpoint", "step", "pause", "Break on start", "debugCommand", "exception"};
    params.set("reason", reasons[static_cast<int>(reason)]).set("callFrames", captured_frames_);
    if (breakpoint) hits.push(std::to_string(breakpoint));
    params.set("hitBreakpoints", hits);
    if (reason == PauseReason::Exception) params.set("data", remote(ctx, exception));
    guard.unlock();
    if (send_event_) send_event_("Debugger.paused", params);
    guard.lock();
    while (paused_ && !stopping_) {
        pause_cv_.wait(guard, [this] { return !paused_ || stopping_ || pending_; });
        if (pending_) {
            auto* request = pending_;
            if (paused_ && request->generation == generation_) {
                preview_bytes_ = 0;
                try { request->result = request->operation(ctx); }
                catch (const std::exception& e) { JS_FreeValue(ctx, JS_GetException(ctx)); request->result = error(e.what()); }
                catch (...) { JS_FreeValue(ctx, JS_GetException(ctx)); request->result = error("Debug inspection failed"); }
            } else request->result = error("Stale paused request");
            request->done = true; pending_ = nullptr; request_cv_.notify_all();
        }
    }
    if (pending_) { pending_->result = error("Invocation resumed"); pending_->done = true; pending_ = nullptr; request_cv_.notify_all(); }
    clear_pause(ctx); paused_ = false;
    guard.unlock();
    if (pause_observer_) pause_observer_(std::chrono::steady_clock::now() - start);
    if (send_event_) send_event_("Debugger.resumed", json::Value::object());
}
int DebugSession::debug_trace_handler(JSContext* ctx, JSAtom filename, JSAtom funcname, int line, int col, int flags, void* opaque) {
    auto* self = static_cast<DebugSession*>(opaque);
    if (!self || !self->enabled_ || self->stopping_ || line <= 0) return 0;
    try {
        const auto file = atom_text(ctx, filename), name = atom_text(ctx, funcname);
        const auto depth = JS_GetStackDepth(ctx);
        PauseReason reason = PauseReason::Step; bool pause = false; int bp = 0;
        {
            std::lock_guard<std::mutex> guard(self->mutex_);
            if (self->entry_pending_) { self->entry_pending_ = false; reason = PauseReason::Entry; pause = true; }
            else if (flags & JS_DEBUG_TRACE_DEBUGGER_STMT) { reason = PauseReason::DebuggerStatement; pause = true; }
            else if (self->pause_requested_) { self->pause_requested_ = false; reason = PauseReason::PauseRequest; pause = true; }
            else if ((self->step_mode_ == StepMode::Into && (file != self->step_file_ || line != self->step_line_ || depth != self->step_depth_))
                || (self->step_mode_ == StepMode::Over && depth <= self->step_depth_ && (file != self->step_file_ || line != self->step_line_ || depth < self->step_depth_))
                || (self->step_mode_ == StepMode::Out && depth < self->step_depth_)) pause = true;
            else if (self->breakpoints_active_) for (const auto& item : self->breakpoints_) {
                const auto& point = item.second;
                if (point.line == line && (same_path(point.url, normalize_url(file)) || point.script_id == self->find_script_id(file))) {
                    reason = PauseReason::Breakpoint; pause = true; bp = point.id; break;
                }
            }
        }
        if (pause) self->do_pause(ctx, file, name, line, col, reason, bp);
    } catch (...) {
        self->on_disconnect();
        std::lock_guard<std::mutex> guard(self->mutex_);
        self->clear_pause(ctx);
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return 0;
}
void DebugSession::exception_handler(JSContext* ctx, JSValueConst exception, bool caught, void* opaque) {
    auto* self = static_cast<DebugSession*>(opaque);
    if (!self || !self->enabled_ || self->stopping_) return;
    {
        std::lock_guard<std::mutex> guard(self->mutex_);
        if (self->exception_policy_ == "none" || (self->exception_policy_ == "uncaught" && caught)) return;
    }
    try {
        JSAtom file = JS_ATOM_NULL, name = JS_ATOM_NULL; int line = 0, col = 0;
        JS_GetDebugFrame(ctx, 0, &file, &name, &line, &col);
        self->do_pause(ctx, atom_text(ctx, file), atom_text(ctx, name), line, col, PauseReason::Exception, 0, exception);
    } catch (...) {
        self->on_disconnect();
        std::lock_guard<std::mutex> guard(self->mutex_);
        self->clear_pause(ctx);
    }
}
