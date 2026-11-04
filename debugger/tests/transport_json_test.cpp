// Standalone: does not initialize or link QuickJS or the example debugger host.
#include "../src/cdp/json.h"
#include "../src/cdp/websocket_server.h"
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
static const char* current_case = "initialization";

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Function> static void rejects(Function action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, "Expected rejection");
}
static void json_tests() {
    using json::Value;
    require(Value::parse(" {\"a\":[true,false,null,-2.5e+2,\"\\uD83D\\uDE00\"]} ")["a"].size() == 5, "JSON valid");
    require(Value::parse("\"\\uD83D\\uDE00\"").get_string() == "\xf0\x9f\x98\x80", "Surrogate pair");
    require(Value::parse(Value(std::string("a\0b", 3)).serialize()).get_string() == std::string("a\0b", 3), "NUL round trip");
    for (const std::string text : {"", " ", "nullx", "true false", "{", "[", "{\"a\" 1}", "{\"a\":1,}",
        "[1,]", "[01]", "1.", "1e", "1e+", "+1", "--1", "nan", "Infinity", "1e999",
        "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\u0000\"", "\"\\uGGGG\"", "\"\\q\"",
        "\"raw\nnewline\"", "{\"a\":1,\"\\u0061\":2}", "[null null]", "{\"a\":}"}) {
        rejects([&] { Value::parse(text); });
    }
    for (const std::string text : {std::string("\"\xc0\xaf\""), std::string("\"\xed\xa0\x80\""),
         std::string("\"\xf4\x90\x80\x80\""), std::string("\"\x80\""), std::string("\"\xe2\x82\"")}) {
        rejects([&] { Value::parse(text); });
        rejects([&] { Value(text.substr(1, text.size() - 2)).serialize(); });
    }
    const std::string depth_ok(64, '['), depth_close(64, ']');
    require(Value::parse(depth_ok + "0" + depth_close).is_array(), "64 JSON containers");
    rejects([&] { Value::parse("[" + depth_ok + "0" + depth_close + "]"); });
    std::string nodes = "[";
    for (size_t i = 0; i < 16383; ++i) { if (i) nodes += ','; nodes += '0'; }
    nodes += ']';
    require(Value::parse(nodes).size() == 16383, "Node boundary");
    nodes.insert(1, "0,");
    rejects([&] { Value::parse(nodes); });
    require(Value::parse("\"" + std::string(Value::max_input_bytes - 2, 'a') + "\"").is_string(), "JSON input byte boundary");
    rejects([&] { Value::parse("\"" + std::string(Value::max_input_bytes, 'a') + "\""); });
    rejects([&] { Value(std::numeric_limits<double>::infinity()).serialize(); });
    rejects([&] { Value(std::numeric_limits<double>::quiet_NaN()).serialize(); });
    rejects([&] { Value(1e300).get_int64(); });
    rejects([&] { Value(2147483648.0).get_int(); });
    require(Value::parse(Value(1e300).serialize()).get_number() == 1e300, "Large finite JSON");
    require(Value::parse(Value(-0.0).serialize()).get_number() == 0, "Negative zero JSON");
    require(Value(std::string(Value::max_output_bytes - 2, 'a')).serialize().size() == Value::max_output_bytes, "JSON output byte boundary");
    rejects([&] { Value(std::string(Value::max_output_bytes, 'a')).serialize(); });
    require(Value::parse("\"\\u20ac\"").serialize() == "\"\xe2\x82\xac\"", "UTF-8 preserved");
}
struct Client {
    socket_t socket = INVALID_SOCK;
    explicit Client(int port) {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(socket != INVALID_SOCK, "Create client socket");
#ifdef _WIN32
        const DWORD timeout = 2000;
#else
        const timeval timeout{2, 0};
#endif
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        require(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Connect loopback client");
    }
    ~Client() { if (socket != INVALID_SOCK) close_socket(socket); }
    void send(const std::string& text) {
        size_t offset = 0;
        while (offset < text.size()) {
            const int count = ::send(socket, text.data() + offset,
                static_cast<int>((std::min)(text.size() - offset, size_t{65536})), 0);
            require(count > 0, "Client send");
            offset += count;
        }
    }
    std::string headers() {
        std::string text;
        char byte;
        while (text.size() < 16384 && recv(socket, &byte, 1, 0) == 1) {
            text += byte;
            if (text.size() >= 4 && text.substr(text.size() - 4) == "\r\n\r\n") return text;
        }
        return text;
    }
    bool closed() {
        char byte;
        const int count = recv(socket, &byte, 1, 0);
        if (count == 0) return true;
        if (count > 0) return false;
#ifdef _WIN32
        return platform_socket_error() == WSAECONNRESET || platform_socket_error() == WSAENOTCONN;
#else
        return platform_socket_error() == ECONNRESET || platform_socket_error() == ENOTCONN;
#endif
    }
    std::string read(size_t length) {
        std::string result(length, '\0');
        size_t offset = 0;
        while (offset < length) {
            const int count = recv(socket, result.data() + offset, static_cast<int>(length - offset), 0);
            require(count > 0, "Read client bytes");
            offset += static_cast<size_t>(count);
        }
        return result;
    }
    std::string frame(const std::string& payload, uint8_t opcode = 1, bool final = true, bool masked = true) {
        std::string result;
        result += static_cast<char>((final ? 0x80 : 0) | opcode);
        const uint8_t mask_flag = masked ? 0x80 : 0;
        if (payload.size() < 126) result += static_cast<char>(mask_flag | payload.size());
        else if (payload.size() <= 65535) {
            result += static_cast<char>(mask_flag | 126);
            result += static_cast<char>(payload.size() >> 8);
            result += static_cast<char>(payload.size());
        } else {
            result += static_cast<char>(mask_flag | 127);
            const uint64_t length = payload.size();
            for (int i = 7; i >= 0; --i) result += static_cast<char>(length >> (i * 8));
        }
        constexpr char mask[4] = {1, 2, 3, 4};
        if (masked) result.append(mask, 4);
        for (size_t i = 0; i < payload.size(); ++i) result += static_cast<char>(masked ? payload[i] ^ mask[i % 4] : payload[i]);
        return result;
    }
    void upgrade(int port, const std::string& extra = "") {
        send("GET /debug HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n");
        const auto response = headers();
        require(response.find("101 Switching Protocols") != std::string::npos, "WS upgrade");
        require(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos, "RFC handshake hash");
    }
};
template<class T> static T bounded(std::future<T>& result) {
    if (result.wait_for(2s) != std::future_status::ready) {
        std::cerr << "FAIL: asynchronous socket operation exceeded two seconds in " << current_case << '\n';
        std::_Exit(2); // Do not let a future destructor wait forever on a regression.
    }
    return result.get();
}
static void transport_tests() {
    // Accept/HTTP/WS I/O all stop promptly without a send mutex or socket-reuse race.
    {
        WebSocketServer server;
        current_case = "Start";
        require(server.start(0), "Start");
        WebSocketServer occupied;
        require(!occupied.start(server.port()), "Exclusive listener port");
        auto waiting = std::async(std::launch::async, [&] { return server.wait_for_connection("Twill", "twill:///test"); });
        server.stop();
        require(!bounded(waiting), "Stop accept");
    }
    {
        WebSocketServer server;
        current_case = "Start HTTP stop";
        require(server.start(0), "Start HTTP stop");
        auto waiting = std::async(std::launch::async, [&] { return server.wait_for_connection("Twill", "twill:///test"); });
        Client client(server.port());
        client.send("GET /debug HTTP/1.1\r\n");
        server.stop();
        require(!bounded(waiting), "Stop incomplete HTTP");
    }
    for (const std::string extra : {
        std::string("Origin: https://untrusted.example\r\n"),
        std::string("Sec-WebSocket-Key: duplicate\r\n"),
        std::string("Sec-WebSocket-Version: 12\r\n"),
        std::string("X-Fill: ") + std::string(8192, 'a') + "\r\n"}) {
        current_case = "Reject invalid HTTP upgrade";
        WebSocketServer target;
        require(target.start(0), "Start invalid HTTP test");
        auto waiting = std::async(std::launch::async, [&] { return target.wait_for_connection("Twill", "test"); });
        Client client(target.port());
        client.send("GET /debug HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(target.port()) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n");
        require(client.closed(), "Reject origin/duplicate headers/oversized HTTP");
        target.stop();
        require(!bounded(waiting), "Stop invalid HTTP listener");
    }
    WebSocketServer server;
    current_case = "Start reusable listener";
    require(server.start(0), "Start reusable listener");
    const int port = server.port();
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto waiting = std::async(std::launch::async, [&] { return server.wait_for_connection("Twill \"test\"", "twill:///test"); });
        {
            Client discovery(port);
            discovery.send("GET /json/list HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n");
            require(discovery.headers().find("200 OK") != std::string::npos, "Discovery response");
        }
        Client client(port);
        client.upgrade(port);
        require(bounded(waiting), "Connect");
        auto message = std::async(std::launch::async, [&] { return server.receive(); });
        client.send(client.frame("hel", 1, false) + client.frame("ping", 9) + client.frame("lo", 0));
        require(bounded(message) == "hello", "Fragmentation + ping");
        // A pong has been written without recursive receive or held mutex deadlock.
        const auto pong = client.read(6);
        require(static_cast<unsigned char>(pong[0]) == 0x8a && pong.substr(2) == "ping", "Pong");
        if (iteration == 0) {
            auto blocked = std::async(std::launch::async, [&] { return server.receive(); });
            server.disconnect();
            require(bounded(blocked).empty(), "Disconnect blocked receive");
        } else if (iteration == 1) {
            auto malformed = std::async(std::launch::async, [&] { return server.receive(); });
            client.send(client.frame("unmasked", 1, true, false));
            require(bounded(malformed).empty(), "Unmasked input rejected");
            require(!server.is_connected(), "Malformed input disconnected");
        } else {
            auto blocked = std::async(std::launch::async, [&] { return server.receive(); });
            server.stop();
            require(bounded(blocked).empty(), "Stop blocked receive");
        }
    }
    // Length validation rejects an oversized declaration without reading/allocating its body.
    for (const std::string invalid : {
        std::string("\x81\xff\x00\x00\x00\x00\x00\x10\x00\x01", 10),
        std::string("\x81\xff\x80\x00\x00\x00\x00\x00\x00\x00", 10),
        std::string("\x09\x80\x01\x02\x03\x04", 6),
        std::string("\xc1\x80\x01\x02\x03\x04", 6),
        std::string("\x81\xfe\x00\x01", 4),
        std::string("\x80\x80\x01\x02\x03\x04", 6)}) {
        WebSocketServer target;
        current_case = "Start malformed input target";
        require(target.start(0), "Start malformed input target");
        auto waiting = std::async(std::launch::async, [&] { return target.wait_for_connection("Twill", "test"); });
        Client client(target.port());
        client.upgrade(target.port());
        require(bounded(waiting), "Connect malformed test");
        auto result = std::async(std::launch::async, [&] { return target.receive(); });
        client.send(invalid);
        require(bounded(result).empty() && !target.is_connected(), "Reject invalid frame header");
        target.stop();
    }
    {
        WebSocketServer target;
        current_case = "Start aggregate budget test";
        require(target.start(0), "Start aggregate budget test");
        auto waiting = std::async(std::launch::async, [&] { return target.wait_for_connection("Twill", "test"); });
        Client client(target.port()); client.upgrade(target.port()); require(bounded(waiting), "Connect aggregate test");
        auto result = std::async(std::launch::async, [&] { return target.receive(); });
        client.send(client.frame(std::string(1024 * 1024, 'a'), 1, false) + client.frame("b", 0));
        require(bounded(result).empty() && !target.is_connected(), "Fragment aggregate budget");
        target.stop();
    }
    {
        WebSocketServer target;
        current_case = "Start incoming boundary test";
        require(target.start(0), "Start incoming boundary test");
        auto waiting = std::async(std::launch::async, [&] { return target.wait_for_connection("Twill", "test"); });
        Client client(target.port()); client.upgrade(target.port()); require(bounded(waiting), "Connect input boundary");
        auto result = std::async(std::launch::async, [&] { return target.receive(); });
        const std::string payload(1024 * 1024, 'a');
        client.send(client.frame(payload));
        require(bounded(result) == payload, "Exactly one MiB input accepted");
        auto bad_text = std::async(std::launch::async, [&] { return target.receive(); });
        client.send(client.frame(std::string("\xc0\xaf", 2)));
        require(bounded(bad_text).empty() && !target.is_connected(), "Invalid text UTF-8 rejected");
        target.stop();
    }
    {
        WebSocketServer target;
        current_case = "Start blocked send test";
        require(target.start(0), "Start blocked send test");
        auto waiting = std::async(std::launch::async, [&] { return target.wait_for_connection("Twill", "test"); });
        Client client(target.port()); client.upgrade(target.port()); require(bounded(waiting), "Connect blocked sender");
        const int small_buffer = 1024;
        setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF,
            reinterpret_cast<const char*>(&small_buffer), sizeof(small_buffer));
        auto sending = std::async(std::launch::async, [&] { return target.send(std::string(4 * 1024 * 1024, 'a')); });
        const bool blocked = sending.wait_for(50ms) == std::future_status::timeout;
        target.stop();
        bounded(sending);
        require(blocked, "Backpressure actually blocked sender before stop");
    }
    {
        WebSocketServer target;
        current_case = "Start output budget test";
        require(target.start(0), "Start output budget test");
        auto waiting = std::async(std::launch::async, [&] { return target.wait_for_connection("Twill", "test"); });
        Client client(target.port()); client.upgrade(target.port()); require(bounded(waiting), "Connect output test");
        require(!target.send(std::string(4 * 1024 * 1024 + 1, 'a')) && !target.is_connected(), "Output budget");
        target.stop();
    }
}
int main() {
    try {
        json_tests();
        transport_tests();
        std::cout << "PASS transport/JSON strict parsing, budgets, masked/fragmented frames, reconnect and bounded stop\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
