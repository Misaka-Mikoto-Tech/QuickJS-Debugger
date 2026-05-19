#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "debug_session.h"

namespace {

bool has_self_object_property(DebugSession& session, const json::Value& params) {
    const json::Value& frames = params["callFrames"];
    if (!frames.is_array() || frames.size() == 0) {
        return false;
    }

    const json::Value& scope_chain = frames[0]["scopeChain"];
    if (!scope_chain.is_array() || scope_chain.size() == 0) {
        return false;
    }

    const std::string scope_object_id = scope_chain[0]["object"]["objectId"].get_string();
    json::Value scope_properties = session.get_properties(scope_object_id);
    const json::Value& properties = scope_properties["result"];
    if (!properties.is_array()) {
        return false;
    }

    for (const auto& property : properties.get_array()) {
        if (!property.is_object() || property["name"].get_string() != "obj") {
            continue;
        }

        const std::string object_id = property["value"]["objectId"].get_string();
        json::Value object_properties = session.get_properties(object_id);
        const json::Value& object_result = object_properties["result"];
        if (!object_result.is_array()) {
            return false;
        }

        for (const auto& object_property : object_result.get_array()) {
            if (object_property["name"].get_string() == "self") {
                const json::Value& self_value = object_property["value"];
                return self_value.is_object() &&
                       self_value["type"].get_string() == "object" &&
                       self_value.has("objectId");
            }
        }
    }

    return false;
}

} // namespace

int main() {
    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) {
        std::cerr << "failed to create JS runtime\n";
        return EXIT_FAILURE;
    }

    JSContext* context = JS_NewContext(runtime);
    if (!context) {
        std::cerr << "failed to create JS context\n";
        JS_FreeRuntime(runtime);
        return EXIT_FAILURE;
    }

    DebugSession session;
    bool paused = false;
    bool captured_self_reference = false;

    session.set_enabled(true);
    session.add_script("cycle-test.js", "function run() {\n  const obj = {};\n  obj.self = obj;\n  return obj;\n}\nrun();\n");
    session.set_breakpoint_by_url("cycle-test.js", 3, 0);
    session.set_send_event([&](const std::string& method, const json::Value& params) {
        if (method == "Debugger.paused") {
            paused = true;
            captured_self_reference = has_self_object_property(session, params);
            session.resume();
        }
    });

    JS_SetDebugTraceHandler(context, DebugSession::debug_trace_handler, &session);

    const char* source = "function run() {\n  const obj = {};\n  obj.self = obj;\n  return obj;\n}\nrun();\n";
    JSValue value = JS_Eval(context, source, std::strlen(source), "cycle-test.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        std::cerr << "failed to evaluate breakpoint script\n";
        JS_FreeValue(context, value);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        return EXIT_FAILURE;
    }

    JS_FreeValue(context, value);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);

    if (!paused || !captured_self_reference) {
        std::cerr << "self-referential object was not captured as a bounded remote object\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}