#pragma once

#include "platform.h"
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

class WebSocketServer {
public:
    WebSocketServer();
    ~WebSocketServer();

    bool start(int port);
    void stop();

    // Accept loop: handles HTTP discovery requests, then upgrades to WebSocket.
    // Blocks until a WebSocket client connects.
    bool wait_for_connection(const std::string& target_name, const std::string& target_url);

    // Thread-safe send (can be called from any thread)
    bool send(const std::string& message);

    // Blocking receive. Returns empty string on error/close.
    std::string receive();

    bool is_connected() const { return connected_.load(); }
    int port() const { return port_.load(); }

    void disconnect();

private:
    // Outstanding I/O owns its socket; stop shuts it down but never closes/reuses
    // its descriptor while another thread still holds a snapshot.
    struct Network;
    struct Socket;
    using SocketPtr = std::shared_ptr<Socket>;
    SocketPtr client_snapshot();
    void close_client(const SocketPtr& client);
    bool send_http_response(const SocketPtr& sock, int code, const std::string& status,
                            const std::string& content_type, const std::string& body);
    bool do_websocket_handshake(const SocketPtr& sock, const std::string& request);
    bool send_ws_frame(const SocketPtr& sock, uint8_t opcode, const uint8_t* data, size_t len);
    bool read_ws_frame(const SocketPtr& sock, std::string& payload, uint8_t& opcode, bool& final);
    bool recv_exact(const SocketPtr& sock, void* buf, size_t len);
    bool send_all(const SocketPtr& sock, const void* buf, size_t len);
    std::string read_http_request(const SocketPtr& sock);

    SocketPtr listen_sock_;
    SocketPtr client_sock_;
    std::atomic<int> port_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopped_{true};
    std::shared_ptr<Network> network_;
    std::mutex state_mutex_;
    std::mutex send_mutex_;
    std::string target_name_;
    std::string target_url_;
};
