/*
 * Client for the lsend background daemon (Unix socket, JSON lines).
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "json_lite.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lsend {

struct Device {
    std::string alias;
    std::string ip;
    std::string fingerprint;
    std::string device_type;
    std::string device_model;
    int port{0};
};

enum class LinkState {
    Connecting,   // trying to reach the daemon
    Starting,     // daemon spawned, waiting for it to come up
    Connected,
    Failed,       // could not start lsend (not installed?)
};

class Client {
public:
    using EventHandler = std::function<void(const json::Value& event)>;

    // socket_path empty -> $LSEND_SOCKET or ~/.config/lsend/ctl.sock
    explicit Client(std::string socket_path = {});
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void set_event_handler(EventHandler handler) { handler_ = std::move(handler); }

    // Call periodically from the UI loop: (re)connects, reads, dispatches events.
    void poll();

    LinkState link_state() const { return link_; }
    const std::vector<Device>& devices() const { return devices_; }
    const std::string& alias() const { return alias_; }
    const std::string& ip() const { return ip_; }
    const std::string& download_dir() const { return download_dir_; }
    bool auto_accept() const { return auto_accept_; }

    void request_state();
    void scan();
    void send_files(const std::string& fingerprint, const std::vector<std::string>& paths);
    void send_text(const std::string& fingerprint, const std::string& text);
    void answer(int id, bool accept);
    void set_auto_accept(bool enabled);

    // Feed a protocol line directly (used by tests / UI previews).
    void inject_line(const std::string& line);

private:
    bool try_connect();
    void spawn_daemon();
    void disconnect();
    void write_line(const std::string& line);
    void handle_line(const std::string& line);
    void apply_devices(const json::Value& list);

    std::string socket_path_;
    int fd_{-1};
    LinkState link_{LinkState::Connecting};
    bool spawned_{false};
    uint32_t last_attempt_ms_{0};
    uint32_t spawn_ms_{0};
    std::string rx_;
    EventHandler handler_;

    std::vector<Device> devices_;
    std::string alias_;
    std::string ip_;
    std::string download_dir_;
    bool auto_accept_{false};
};

} // namespace lsend
