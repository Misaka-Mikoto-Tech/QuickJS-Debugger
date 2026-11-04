#include "cdp_handler.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace {
std::string text(const json::Value& value, const char* key) {
    return value[key].is_string() ? value[key].get_string() : "";
}
int integer(const json::Value& value, const char* key, int fallback = -1) {
    const auto& number = value[key];
    if (!number.is_number() || !std::isfinite(number.get_number()) || std::floor(number.get_number()) != number.get_number()
        || number.get_number() < 0 || number.get_number() > 2147483646.0) return fallback;
    return number.get_int();
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \r\n\t"), last = value.find_last_not_of(" \r\n\t");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}
bool field_operation(const json::Value& params, std::string& field, json::Value& value) {
    const auto function = trim(text(params, "functionDeclaration"));
    const auto& arguments = params["arguments"];
    if (function.size() > 32768 || !arguments.is_array() || arguments.size() < 1
        || !arguments[0]["value"].is_string()) return false;
    field = arguments[0]["value"].get_string();
    if (field.size() > 1024) return false;
    std::string compact;
    for (char c : function) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') compact += c;
    if (compact == "function(a,b){this[a]=b;}" && arguments.size() == 2) { value = arguments[1]; return true; }
    // js-debug's single-argument helper is parsed as DATA, not executed.
    if (arguments.size() != 1) return false;
    static const std::regex prefix(R"(^function\s*\(a\)\s*\{\s*return\s+this\[a\]\s*=\s*)");
    std::smatch match;
    if (!std::regex_search(function, match, prefix)) return false;
    const auto begin = static_cast<size_t>(match.length());
    const auto comment = function.find("\n//# sourceURL=eval-", begin);
    if (comment == std::string::npos || function.find("\n//# sourceURL=", comment + 1) != std::string::npos) return false;
    static const std::regex suffix(R"(^\n//# sourceURL=eval-[0-9a-fA-F]{8}\.cdp\r?\n[ \t]*\}$)");
    if (!std::regex_match(function.substr(comment), suffix)) return false;
    auto literal = trim(function.substr(begin, comment - begin));
    if (literal.empty() || literal.back() != ';') return false;
    literal.pop_back();
    return DebugSession::parse_literal(literal, value);
}
}

CDPHandler::CDPHandler(WebSocketServer& ws, DebugSession& session) : ws_(ws), session_(session) {}
void CDPHandler::send_response(int id, const json::Value& result) {
    auto message = json::Value::object(); message.set("id", id).set("result", result);
    if (!ws_.send(message.serialize())) session_.on_disconnect();
}
void CDPHandler::send_error(int id, int code, const std::string& reason) {
    auto message = json::Value::object(), error = json::Value::object();
    error.set("code", code).set("message", reason); message.set("id", id).set("error", error);
    if (!ws_.send(message.serialize())) session_.on_disconnect();
}
void CDPHandler::send_event(const std::string& method, const json::Value& params) {
    try {
        auto message = json::Value::object(); message.set("method", method).set("params", params);
        if (!ws_.send(message.serialize())) session_.on_disconnect();
    } catch (...) { ws_.disconnect(); session_.on_disconnect(); }
}
void CDPHandler::handle_message(const std::string& message) {
    int id = 0;
    try {
        const auto request = json::Value::parse(message);
        if (!request.is_object()) { send_error(0, -32600, "Expected CDP request object"); return; }
        id = integer(request, "id", 0);
        const auto method = text(request, "method");
        const auto params = request.has("params") ? request["params"] : json::Value::object();
        if (!params.is_object()) { send_error(id, -32602, "Expected object params"); return; }
        json::Value result;
        if (method == "Debugger.enable") result = on_debugger_enable(params);
        else if (method == "Debugger.disable") result = on_debugger_disable(params);
        else if (method == "Debugger.setBreakpointByUrl") result = on_debugger_set_breakpoint_by_url(params);
        else if (method == "Debugger.removeBreakpoint") result = on_debugger_remove_breakpoint(params);
        else if (method == "Debugger.resume") result = on_debugger_resume(params);
        else if (method == "Debugger.stepOver") result = on_debugger_step_over(params);
        else if (method == "Debugger.stepInto") result = on_debugger_step_into(params);
        else if (method == "Debugger.stepOut") result = on_debugger_step_out(params);
        else if (method == "Debugger.pause") result = on_debugger_pause(params);
        else if (method == "Debugger.getScriptSource") result = on_debugger_get_script_source(params);
        else if (method == "Debugger.setBreakpointsActive") result = on_debugger_set_breakpoints_active(params);
        else if (method == "Debugger.getPossibleBreakpoints") result = on_debugger_get_possible_breakpoints(params);
        else if (method == "Debugger.evaluateOnCallFrame") result = on_debugger_evaluate_on_call_frame(params);
        else if (method == "Debugger.setVariableValue") result = on_debugger_set_variable_value(params);
        else if (method == "Debugger.setPauseOnExceptions") result = on_debugger_set_pause_on_exceptions(params);
        else if (method == "Runtime.enable") result = on_runtime_enable(params);
        else if (method == "Runtime.disable") result = on_runtime_disable(params);
        else if (method == "Runtime.getProperties") result = on_runtime_get_properties(params);
        else if (method == "Runtime.runIfWaitingForDebugger") result = on_runtime_run_if_waiting(params);
        else if (method == "Runtime.evaluate") result = on_runtime_evaluate(params);
        else if (method == "Runtime.callFunctionOn") result = on_runtime_call_function_on(params);
        else if (method == "Runtime.releaseObjectGroup") result = on_runtime_release_object_group(params);
        else if (method == "Runtime.getIsolateId") result = on_runtime_get_isolate_id(params);
        else if (method == "Runtime.releaseObject" || method == "Debugger.setAsyncCallStackDepth" ||
                 method == "Runtime.setCustomObjectFormatterEnabled" || method == "Profiler.enable" || method == "Profiler.disable")
            result = json::Value::object();
        else result = DebugSession::error("Unsupported CDP operation; arbitrary code execution is disabled");
        if (id > 0) {
            if (result.has("error")) send_error(id, -32000, text(result["error"], "message"));
            else send_response(id, result);
        }
    } catch (const std::exception&) {
        try { send_error(id, -32602, "Invalid or over-budget CDP request/response"); }
        catch (...) { ws_.disconnect(); session_.on_disconnect(); }
    } catch (...) { ws_.disconnect(); session_.on_disconnect(); }
}
json::Value CDPHandler::on_debugger_enable(const json::Value&) {
    debugger_enabled_ = true; session_.set_enabled(true);
    for (const auto& script : session_.scripts()) send_event("Debugger.scriptParsed", session_.script_parsed(script));
    auto result = json::Value::object(); result.set("debuggerId", "quickjs-debugger-1"); return result;
}
json::Value CDPHandler::on_debugger_disable(const json::Value&) { debugger_enabled_ = false; session_.set_enabled(false); return json::Value::object(); }
json::Value CDPHandler::on_debugger_set_breakpoint_by_url(const json::Value& params) {
    if (params.has("condition") && (!params["condition"].is_string() || !params["condition"].get_string().empty())) return DebugSession::error("Conditional breakpoints are unsupported");
    return session_.set_breakpoint_by_url(text(params, "url"), integer(params, "lineNumber"), integer(params, "columnNumber", 0), params.has("urlRegex"));
}
json::Value CDPHandler::on_debugger_remove_breakpoint(const json::Value& params) {
    if (!session_.remove_breakpoint(text(params, "breakpointId"))) return DebugSession::error("Unknown breakpoint");
    return json::Value::object();
}
json::Value CDPHandler::on_debugger_resume(const json::Value&) { session_.resume(); return json::Value::object(); }
json::Value CDPHandler::on_debugger_step_over(const json::Value&) { session_.step_over(); return json::Value::object(); }
json::Value CDPHandler::on_debugger_step_into(const json::Value&) { session_.step_into(); return json::Value::object(); }
json::Value CDPHandler::on_debugger_step_out(const json::Value&) { session_.step_out(); return json::Value::object(); }
json::Value CDPHandler::on_debugger_pause(const json::Value&) { session_.request_pause(); return json::Value::object(); }
json::Value CDPHandler::on_debugger_get_script_source(const json::Value& params) {
    const auto* source = session_.get_script_by_id(text(params, "scriptId"));
    if (!source) return DebugSession::error("Unknown script");
    auto result = json::Value::object(); result.set("scriptSource", source->source); return result;
}
json::Value CDPHandler::on_debugger_set_breakpoints_active(const json::Value& params) {
    if (!params["active"].is_bool()) return DebugSession::error("Expected boolean active");
    session_.set_breakpoints_active(params["active"].get_bool()); return json::Value::object();
}
json::Value CDPHandler::on_debugger_get_possible_breakpoints(const json::Value& params) {
    const auto* source = session_.get_script_by_id(text(params["start"], "scriptId"));
    const int first = integer(params["start"], "lineNumber");
    if (!source || first < 0 || first >= source->end_line) return DebugSession::error("Invalid breakpoint range");
    const int requested_end = params.has("end") ? integer(params["end"], "lineNumber") : first + 1;
    if (requested_end < first || requested_end - first > 256) return DebugSession::error("Breakpoint range budget exceeded");
    auto locations = json::Value::array();
    for (int line = first; line < std::min(requested_end, source->end_line); ++line) {
        auto location = json::Value::object(); location.set("scriptId", source->id).set("lineNumber", line).set("columnNumber", 0); locations.push(location);
    }
    auto result = json::Value::object(); result.set("locations", locations); return result;
}
json::Value CDPHandler::on_debugger_evaluate_on_call_frame(const json::Value& params) {
    return session_.evaluate_on_call_frame(text(params, "callFrameId"), text(params, "expression"));
}
json::Value CDPHandler::on_debugger_set_variable_value(const json::Value& params) {
    if (!params.has("newValue")) return DebugSession::error("Expected typed replacement value");
    return session_.set_variable_value(integer(params, "scopeNumber"), text(params, "variableName"), params["newValue"], text(params, "callFrameId"));
}
json::Value CDPHandler::on_debugger_set_pause_on_exceptions(const json::Value& params) { return session_.set_pause_on_exceptions(text(params, "state")); }
json::Value CDPHandler::on_runtime_enable(const json::Value&) {
    runtime_enabled_ = true;
    auto description = json::Value::object(), aux = json::Value::object(), params = json::Value::object();
    aux.set("isDefault", true); description.set("id", 1).set("origin", "").set("name", "QuickJS").set("auxData", aux);
    params.set("context", description); send_event("Runtime.executionContextCreated", params); return json::Value::object();
}
json::Value CDPHandler::on_runtime_disable(const json::Value&) { runtime_enabled_ = false; return json::Value::object(); }
json::Value CDPHandler::on_runtime_get_properties(const json::Value& params) { return session_.get_properties(text(params, "objectId")); }
json::Value CDPHandler::on_runtime_run_if_waiting(const json::Value&) { session_.run_if_waiting(); return json::Value::object(); }
json::Value CDPHandler::on_runtime_evaluate(const json::Value& params) { return session_.evaluate_literal(text(params, "expression")); }
json::Value CDPHandler::on_runtime_call_function_on(const json::Value& params) {
    std::string field; json::Value argument;
    if (!field_operation(params, field, argument)) return DebugSession::error("Only strict primitive data-field assignment helpers are supported");
    return session_.set_object_value(text(params, "objectId"), field, argument);
}
json::Value CDPHandler::on_runtime_release_object_group(const json::Value&) { return json::Value::object(); }
json::Value CDPHandler::on_runtime_get_isolate_id(const json::Value&) { auto result = json::Value::object(); result.set("id", "quickjs-isolate-1"); return result; }
json::Value CDPHandler::on_profiler_enable(const json::Value&) { return json::Value::object(); }
json::Value CDPHandler::on_profiler_disable(const json::Value&) { return json::Value::object(); }
