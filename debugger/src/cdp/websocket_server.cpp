#include "websocket_server.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <utility>
#include "json.h"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#endif

// ============================================================
// SHA-1 (Windows CryptoAPI for correctness)
// ============================================================
namespace {

void sha1_hash(const void* data, size_t len, uint8_t digest[20]) {
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    memset(digest, 0, 20);
    if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        fprintf(stderr, "[SHA1] CryptAcquireContext failed: %lu\n", GetLastError());
        return;
    }
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        fprintf(stderr, "[SHA1] CryptCreateHash failed: %lu\n", GetLastError());
        CryptReleaseContext(hProv, 0);
        return;
    }
    if (!CryptHashData(hHash, (const BYTE*)data, (DWORD)len, 0)) {
        fprintf(stderr, "[SHA1] CryptHashData failed: %lu\n", GetLastError());
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return;
    }
    DWORD hashLen = 20;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, digest, &hashLen, 0)) {
        fprintf(stderr, "[SHA1] CryptGetHashParam failed: %lu\n", GetLastError());
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
#else
    // Minimal SHA-1 for non-Windows platforms
    auto rol = [](uint32_t v, int b) -> uint32_t { return (v << b) | (v >> (32 - b)); };
    uint32_t h0=0x67452301, h1=0xEFCDAB89, h2=0x98BADCFE, h3=0x10325476, h4=0xC3D2E1F0;
    // Build padded message
    size_t ml = len * 8;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded_len, 0);
    memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++) msg[padded_len - 1 - i] = (uint8_t)(ml >> (i * 8));
    // Process blocks
    for (size_t off = 0; off < padded_len; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)msg[off+i*4]<<24 | (uint32_t)msg[off+i*4+1]<<16 |
                    (uint32_t)msg[off+i*4+2]<<8 | (uint32_t)msg[off+i*4+3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
        uint32_t a=h0, b=h1, c=h2, d=h3, e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f=(b&c)|((~b)&d);     k=0x5A827999; }
            else if (i < 40) { f=b^c^d;               k=0x6ED9EBA1; }
            else if (i < 60) { f=(b&c)|(b&d)|(c&d);   k=0x8F1BBCDC; }
            else              { f=b^c^d;               k=0xCA62C1D6; }
            uint32_t t = rol(a,5)+f+e+k+w[i];
            e=d; d=c; c=rol(b,30); b=a; a=t;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        digest[i*4]=(uint8_t)(h[i]>>24); digest[i*4+1]=(uint8_t)(h[i]>>16);
        digest[i*4+2]=(uint8_t)(h[i]>>8); digest[i*4+3]=(uint8_t)(h[i]);
    }
#endif
}

std::string base64_encode(const uint8_t* data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    r.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i+1 < len) n |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) n |= data[i+2];
        r += table[(n>>18)&63];
        r += table[(n>>12)&63];
        r += (i+1 < len) ? table[(n>>6)&63] : '=';
        r += (i+2 < len) ? table[n&63] : '=';
    }
    return r;
}

std::string compute_ws_accept(const std::string& key) {
    std::string concat = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    sha1_hash(concat.data(), concat.size(), digest);
    return base64_encode(digest, 20);
}

std::string ascii_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(std::string text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// Match complete header names, reject ambiguous duplicates and folded lines.
std::string get_header(const std::string& request, const std::string& name) {
    std::string result;
    bool found = false;
    size_t pos = request.find("\r\n");
    if (pos == std::string::npos) return {};
    pos += 2;
    while (pos < request.size()) {
        const size_t end = request.find("\r\n", pos);
        if (end == std::string::npos || end == pos) break;
        const std::string line = request.substr(pos, end - pos);
        if (line[0] == ' ' || line[0] == '\t') return {};
        const size_t colon = line.find(':');
        if (colon != std::string::npos && ascii_lower(line.substr(0, colon)) == ascii_lower(name)) {
            if (found) return {};
            found = true;
            result = trim(line.substr(colon + 1));
        }
        pos = end + 2;
    }
    return result;
}

bool valid_http_headers(const std::string& request) {
    std::map<std::string, bool> names;
    size_t pos = request.find("\r\n");
    if (pos == std::string::npos) return false;
    pos += 2;
    while (pos < request.size()) {
        const size_t end = request.find("\r\n", pos);
        if (end == std::string::npos) return false;
        if (end == pos) return end + 2 == request.size();
        const std::string line = request.substr(pos, end - pos);
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        for (size_t i = 0; i < colon; ++i) {
            const auto c = static_cast<unsigned char>(line[i]);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || std::string("!#$%&'*+-.^_\x60|~").find(static_cast<char>(c)) != std::string::npos))
                return false;
        }
        for (size_t i = colon + 1; i < line.size(); ++i) {
            const auto c = static_cast<unsigned char>(line[i]);
            if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
        }
        if (!names.emplace(ascii_lower(line.substr(0, colon)), true).second) return false;
        pos = end + 2;
    }
    return false;
}

bool header_token(const std::string& text, const std::string& token) {
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find(',', start);
        if (ascii_lower(trim(text.substr(start, end - start))) == token) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

std::string get_request_path(const std::string& request) {
    if (request.rfind("GET ", 0) != 0) return {};
    const size_t end = request.find(' ', 4);
    if (end == std::string::npos || request.compare(end, 11, " HTTP/1.1\r\n") != 0) return {};
    return request.substr(4, end - 4);
}

bool valid_ws_key(const std::string& key) {
    if (key.size() != 24 || key.substr(22) != "==") return false;
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < 22; ++i)
        if (alphabet.find(key[i]) == std::string::npos) return false;
    return (alphabet.find(key[21]) & 15) == 0;
}

bool set_blocking(socket_t socket, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK) == 0;
#endif
}

bool wait_readable(socket_t socket, long milliseconds) {
    fd_set sockets;
    FD_ZERO(&sockets);
    FD_SET(socket, &sockets);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    return select(static_cast<int>(socket) + 1, &sockets, nullptr, nullptr, &timeout) > 0;
}

bool wait_writable(socket_t socket, long milliseconds) {
    fd_set sockets;
    FD_ZERO(&sockets);
    FD_SET(socket, &sockets);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    return select(static_cast<int>(socket) + 1, nullptr, &sockets, nullptr, &timeout) > 0;
}

bool retry_socket_error() {
    const int error = platform_socket_error();
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return error == EWOULDBLOCK || error == EAGAIN || error == EINTR;
#endif
}

} // anonymous namespace

// ============================================================
// WebSocketServer implementation
// ============================================================

struct WebSocketServer::Network {
    const bool initialized = platform_socket_init();
    ~Network() { if (initialized) platform_socket_cleanup(); }
};

struct WebSocketServer::Socket {
    socket_t handle;
    std::shared_ptr<Network> network;
    std::atomic<bool> closed{false};

    Socket(socket_t value, std::shared_ptr<Network> owner) : handle(value), network(std::move(owner)) {}
    void cancel() {
        if (closed.exchange(true)) return;
#ifdef _WIN32
        shutdown(handle, SD_BOTH);
#else
        shutdown(handle, SHUT_RDWR);
#endif
    }
    ~Socket() { cancel(); close_socket(handle); }
};

WebSocketServer::WebSocketServer() = default;
WebSocketServer::~WebSocketServer() { stop(); }

bool WebSocketServer::start(int port) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (listen_sock_ || port < 0 || port > 65535) return false;
    if (!network_) network_ = std::make_shared<Network>();
    if (!network_->initialized) return false;
    const socket_t handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCK) return false;
    auto listener = std::make_shared<Socket>(handle, network_);
    const int option = 1;
#ifdef _WIN32
    // SO_REUSEADDR on Windows would allow another process to hijack this port.
    if (setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&option), sizeof(option)) != 0) return false;
#else
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(handle, 1) != 0 || !set_blocking(handle, false)) return false;
    socklen_t size = sizeof(address);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    port_.store(ntohs(address.sin_port));
    listen_sock_ = std::move(listener);
    stopped_.store(false);
    return true;
}

WebSocketServer::SocketPtr WebSocketServer::client_snapshot() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return client_sock_;
}

void WebSocketServer::close_client(const SocketPtr& client) {
    if (!client) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (client_sock_ == client) {
            connected_.store(false);
            client_sock_.reset();
        }
    }
    client->cancel();
}

void WebSocketServer::stop() {
    SocketPtr client, listener;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stopped_.store(true);
        connected_.store(false);
        client = std::exchange(client_sock_, {});
        listener = std::exchange(listen_sock_, {});
    }
    // No send/I/O lock is held. Nonblocking I/O and bounded readiness waits observe
    // cancellation even on platforms where shutdown does not interrupt a send.
    if (client) client->cancel();
    if (listener) listener->cancel();
}

void WebSocketServer::disconnect() { close_client(client_snapshot()); }

std::string WebSocketServer::read_http_request(const SocketPtr& client) {
    std::string request;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (request.size() < 8192 && !stopped_.load() && !client->closed.load() &&
           std::chrono::steady_clock::now() < deadline) {
        if (!wait_readable(client->handle, 100)) continue;
        char byte = 0;
        const int count = recv(client->handle, &byte, 1, 0);
        if (count < 0 && retry_socket_error()) continue;
        if (count != 1 || byte == '\0') return {};
        request += byte;
        if (request.size() >= 4 && request.compare(request.size() - 4, 4, "\r\n\r\n") == 0)
            return request;
    }
    return {};
}

bool WebSocketServer::wait_for_connection(const std::string& target_name,
                                          const std::string& target_url) {
    if (target_name.size() > 4096 || target_url.size() > 4096) return false;
    target_name_ = target_name;
    target_url_ = target_url;
    SocketPtr listener;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        listener = listen_sock_;
    }
    if (!listener) return false;
    while (!stopped_.load() && !listener->closed.load()) {
        if (!wait_readable(listener->handle, 100)) continue;
        sockaddr_in address{};
        socklen_t size = sizeof(address);
        const socket_t handle = accept(listener->handle, reinterpret_cast<sockaddr*>(&address), &size);
        if (handle == INVALID_SOCK) continue;
        auto client = std::make_shared<Socket>(handle, network_);
        if (ntohl(address.sin_addr.s_addr) != INADDR_LOOPBACK || !set_blocking(handle, false)) continue;
        const int flag = 1;
        setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (stopped_.load() || listen_sock_ != listener) return false;
            client_sock_ = client; // stop can now cancel HTTP/handshake reads as well.
        }
        const std::string request = read_http_request(client);
        const std::string path = get_request_path(request);
        const std::string host = ascii_lower(get_header(request, "Host"));
        const std::string suffix = ":" + std::to_string(port());
        const bool local_host = host == "127.0.0.1" + suffix || host == "localhost" + suffix ||
            (port() == 80 && (host == "127.0.0.1" || host == "localhost"));
        if (path.empty() || !valid_http_headers(request) || !local_host || !get_header(request, "Transfer-Encoding").empty() ||
            (!get_header(request, "Content-Length").empty() && get_header(request, "Content-Length") != "0")) {
            close_client(client);
            continue;
        }
        if (ascii_lower(get_header(request, "Upgrade")) == "websocket") {
            if (path == "/debug" && do_websocket_handshake(client, request)) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!stopped_.load() && client_sock_ == client && !client->closed.load()) {
                    connected_.store(true);
                    return true;
                }
            }
            close_client(client);
            continue;
        }
        if (path == "/json/version") {
            send_http_response(client, 200, "OK", "application/json",
                "{\"Browser\":\"Twill QuickJS\",\"Protocol-Version\":\"1.3\"}");
        } else if (path == "/json" || path == "/json/list") {
            auto target = json::Value::object();
            target.set("description", "Twill embedded QuickJS");
            target.set("id", "main");
            target.set("title", target_name_);
            target.set("type", "node");
            target.set("url", target_url_);
            target.set("webSocketDebuggerUrl", "ws://127.0.0.1:" + std::to_string(port()) + "/debug");
            auto targets = json::Value::array();
            targets.push(target);
            send_http_response(client, 200, "OK", "application/json", targets.serialize());
        } else send_http_response(client, 404, "Not Found", "text/plain", "Not Found");
        close_client(client);
    }
    return false;
}

bool WebSocketServer::send_all(const SocketPtr& client, const void* buffer, size_t length) {
    const char* bytes = static_cast<const char*>(buffer);
    size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sent < length && !stopped_.load() && !client->closed.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (!wait_writable(client->handle, 100)) continue;
        const size_t chunk = (std::min)(length - sent, size_t{65536});
        const int count = ::send(client->handle, bytes + sent, static_cast<int>(chunk), 0);
        if (count < 0 && retry_socket_error()) continue;
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return sent == length;
}

bool WebSocketServer::send_http_response(const SocketPtr& client, int code, const std::string& status,
                                         const std::string& content_type, const std::string& body) {
    if (body.size() > json::Value::max_output_bytes) return false;
    const std::string header = "HTTP/1.1 " + std::to_string(code) + " " + status +
        "\r\nContent-Type: " + content_type + "; charset=UTF-8\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    return send_all(client, header.data(), header.size()) && send_all(client, body.data(), body.size());
}

bool WebSocketServer::do_websocket_handshake(const SocketPtr& client, const std::string& request) {
    const std::string key = get_header(request, "Sec-WebSocket-Key");
    if (!valid_ws_key(key) || get_header(request, "Sec-WebSocket-Version") != "13" ||
        !header_token(get_header(request, "Connection"), "upgrade")) return false;
    // Native CDP clients send no Origin. Do not let arbitrary web origins drive this
    // loopback-only development endpoint through a cross-site WebSocket.
    const std::string origin = ascii_lower(get_header(request, "Origin"));
    if (!origin.empty() && origin != "devtools://devtools" &&
        origin != "http://localhost:" + std::to_string(port()) &&
        origin != "http://127.0.0.1:" + std::to_string(port())) return false;
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: " + compute_ws_accept(key) + "\r\n\r\n";
    return send_all(client, response.data(), response.size());
}

bool WebSocketServer::send(const std::string& message) {
    const auto client = client_snapshot();
    if (!client || !connected_.load()) return false;
    if (message.size() > json::Value::max_output_bytes || !json::Value::is_valid_utf8(message)) {
        close_client(client);
        return false;
    }
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        success = send_ws_frame(client, 0x01, reinterpret_cast<const uint8_t*>(message.data()), message.size());
    }
    if (!success) close_client(client);
    return success;
}

std::string WebSocketServer::receive() {
    const auto client = client_snapshot();
    if (!client || !connected_.load()) return {};
    std::string message;
    bool fragmented = false;
    // Control/empty-fragment floods do not recurse or monopolize this request forever.
    for (size_t frames = 0; frames < 1024 && !stopped_.load(); ++frames) {
        std::string payload;
        uint8_t opcode = 0;
        bool final = false;
        if (!read_ws_frame(client, payload, opcode, final)) break;
        if (opcode == 0x08) {
            if (payload.size() == 1) break;
            if (payload.size() >= 2) {
                const auto code = (static_cast<unsigned char>(payload[0]) << 8) |
                    static_cast<unsigned char>(payload[1]);
                if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 ||
                    code == 1006 || code == 1015 || (code >= 1016 && code < 3000) ||
                    !json::Value::is_valid_utf8(payload.substr(2))) break;
            }
            { std::lock_guard<std::mutex> lock(send_mutex_);
              send_ws_frame(client, 0x08, reinterpret_cast<const uint8_t*>(payload.data()), payload.size()); }
            break;
        }
        if (opcode == 0x09 || opcode == 0x0a) {
            if (opcode == 0x09) {
                std::lock_guard<std::mutex> lock(send_mutex_);
                if (!send_ws_frame(client, 0x0a, reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) break;
            }
            continue;
        }
        if (opcode == 0x01) {
            if (fragmented) break;
            fragmented = !final;
        } else if (opcode == 0x00) {
            if (!fragmented) break;
        } else break; // CDP is text only; binary/unknown extensions are unsupported.
        if (payload.size() > json::Value::max_input_bytes - message.size()) break;
        message += payload;
        if (final) {
            if (!json::Value::is_valid_utf8(message)) break;
            return message;
        }
    }
    close_client(client);
    return {};
}

bool WebSocketServer::send_ws_frame(const SocketPtr& client, uint8_t opcode, const uint8_t* data, size_t len) {
    if (!client || client->closed.load() || len > json::Value::max_output_bytes) return false;
    uint8_t header[10] = {static_cast<uint8_t>(0x80 | opcode)};
    size_t header_len = 2;
    if (len < 126) header[1] = static_cast<uint8_t>(len);
    else if (len < 65536) {
        header[1] = 126;
        header[2] = static_cast<uint8_t>(len >> 8);
        header[3] = static_cast<uint8_t>(len);
        header_len = 4;
    } else {
        header[1] = 127;
        const uint64_t length = len;
        for (int i = 0; i < 8; ++i) header[2 + i] = static_cast<uint8_t>(length >> ((7 - i) * 8));
        header_len = 10;
    }
    return send_all(client, header, header_len) && (!len || send_all(client, data, len));
}

bool WebSocketServer::recv_exact(const SocketPtr& client, void* buffer, size_t length) {
    auto* bytes = static_cast<char*>(buffer);
    size_t received = 0;
    while (received < length && !stopped_.load() && !client->closed.load()) {
        if (!wait_readable(client->handle, 100)) continue;
        const size_t chunk = (std::min)(length - received, size_t{65536});
        const int count = recv(client->handle, bytes + received, static_cast<int>(chunk), 0);
        if (count < 0 && retry_socket_error()) continue;
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    return received == length;
}

bool WebSocketServer::read_ws_frame(const SocketPtr& client, std::string& payload, uint8_t& opcode, bool& final) {
    uint8_t header[2];
    if (!recv_exact(client, header, 2)) return false;
    final = (header[0] & 0x80) != 0;
    opcode = header[0] & 0x0f;
    if ((header[0] & 0x70) != 0 || (header[1] & 0x80) == 0 ||
        (opcode != 0 && opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10)) return false;
    const uint8_t length_code = header[1] & 0x7f;
    uint64_t length = length_code;
    if (length_code == 126) {
        uint8_t extended[2];
        if (!recv_exact(client, extended, 2)) return false;
        length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
        if (length < 126) return false;
    } else if (length_code == 127) {
        uint8_t extended[8];
        if (!recv_exact(client, extended, 8) || (extended[0] & 0x80)) return false;
        length = 0;
        for (const auto byte : extended) length = (length << 8) | byte;
        if (length < 65536) return false;
    }
    if (length > json::Value::max_input_bytes || ((opcode & 8) && (!final || length > 125))) return false;
    uint8_t mask[4];
    if (!recv_exact(client, mask, sizeof(mask))) return false;
    payload.resize(static_cast<size_t>(length));
    if (length && !recv_exact(client, payload.data(), payload.size())) return false;
    for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    return true;
}
