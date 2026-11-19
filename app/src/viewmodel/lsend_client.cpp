/*
 * Client for the lsend background daemon (Unix socket, JSON lines).
 * SPDX-License-Identifier: MIT
 */

#include "lsend_client.h"

#include "lvgl.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lsend {
namespace {

constexpr uint32_t kRetryMs = 1000;
constexpr uint32_t kSpawnTimeoutMs = 20000;  // first run generates a TLS cert

std::string default_socket_path() {
    if (const char* env = std::getenv("LSEND_SOCKET")) {
        if (env[0]) {
            return env;
        }
    }
    const char* home = std::getenv("HOME");
    return std::string(home && home[0] ? home : "/tmp") + "/.config/lsend/ctl.sock";
}

} // namespace

Client::Client(std::string socket_path)
    : socket_path_(socket_path.empty() ? default_socket_path() : std::move(socket_path)) {}

Client::~Client() {
    disconnect();
}

bool Client::try_connect() {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_ = fd;
    rx_.clear();
    return true;
}

void Client::spawn_daemon() {
    // Double fork so the daemon is reparented to init and never becomes a zombie.
    const pid_t child = fork();
    if (child == 0) {
        setsid();
        if (fork() == 0) {
            const int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            execlp("lsend", "lsend", "daemon", static_cast<char*>(nullptr));
            execl("/usr/bin/lsend", "lsend", "daemon", static_cast<char*>(nullptr));
            execl("/usr/local/bin/lsend", "lsend", "daemon", static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    if (child > 0) {
        waitpid(child, nullptr, 0);
    }
    spawned_ = true;
    spawn_ms_ = lv_tick_get();
}

void Client::disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void Client::poll() {
    if (fd_ < 0) {
        if (lv_tick_elaps(last_attempt_ms_) < kRetryMs && last_attempt_ms_ != 0) {
            return;
        }
        last_attempt_ms_ = lv_tick_get();
        if (last_attempt_ms_ == 0) {
            last_attempt_ms_ = 1;
        }
        if (try_connect()) {
            link_ = LinkState::Connected;
            json::Value ev;
            json::parse("{\"type\":\"connected\"}", ev);
            if (handler_) {
                handler_(ev);
            }
            return;
        }
        if (!spawned_) {
            spawn_daemon();
            link_ = LinkState::Starting;
        } else if (link_ == LinkState::Starting && lv_tick_elaps(spawn_ms_) > kSpawnTimeoutMs) {
            link_ = LinkState::Failed;
        }
        return;
    }

    char buf[4096];
    while (true) {
        const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            rx_.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            disconnect();
            // Daemon went away: allow one respawn attempt.
            spawned_ = false;
            link_ = LinkState::Connecting;
            json::Value ev;
            json::parse("{\"type\":\"disconnected\"}", ev);
            if (handler_) {
                handler_(ev);
            }
        }
        break;
    }

    size_t pos;
    while ((pos = rx_.find('\n')) != std::string::npos) {
        std::string line = rx_.substr(0, pos);
        rx_.erase(0, pos + 1);
        handle_line(line);
    }
}

void Client::inject_line(const std::string& line) {
    handle_line(line);
}

void Client::apply_devices(const json::Value& list) {
    devices_.clear();
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& d = list[i];
        Device dev;
        dev.alias = d["alias"].str("?");
        dev.ip = d["ip"].str();
        dev.fingerprint = d["fingerprint"].str();
        dev.device_type = d["deviceType"].str();
        dev.device_model = d["deviceModel"].str();
        dev.port = static_cast<int>(d["port"].integer(53317));
        devices_.push_back(std::move(dev));
    }
}

void Client::handle_line(const std::string& line) {
    json::Value ev;
    if (!json::parse(line, ev) || ev.type != json::Value::Type::Object) {
        return;
    }
    const std::string type = ev["type"].str();
    if (type == "state") {
        alias_ = ev["alias"].str();
        ip_ = ev["ip"].str();
        download_dir_ = ev["download_dir"].str();
        auto_accept_ = ev["auto_accept"].boolean();
        apply_devices(ev["devices"]);
    } else if (type == "devices") {
        apply_devices(ev["devices"]);
    }
    if (handler_) {
        handler_(ev);
    }
}

void Client::write_line(const std::string& line) {
    if (fd_ < 0) {
        return;
    }
    std::string out = line + "\n";
    size_t off = 0;
    int spins = 0;
    while (off < out.size()) {
        const ssize_t n = send(fd_, out.data() + off, out.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++spins < 200) {
            usleep(1000);
        } else {
            disconnect();
            return;
        }
    }
}

void Client::request_state() {
    write_line("{\"cmd\":\"state\"}");
}

void Client::scan() {
    write_line("{\"cmd\":\"scan\"}");
}

void Client::send_files(const std::string& fingerprint, const std::vector<std::string>& paths) {
    std::string line = "{\"cmd\":\"send\",\"to\":" + json::quote(fingerprint) + ",\"paths\":[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i) {
            line += ",";
        }
        line += json::quote(paths[i]);
    }
    line += "]}";
    write_line(line);
}

void Client::send_text(const std::string& fingerprint, const std::string& text) {
    write_line("{\"cmd\":\"send\",\"to\":" + json::quote(fingerprint) +
               ",\"text\":" + json::quote(text) + "}");
}

void Client::answer(int id, bool accept) {
    write_line("{\"cmd\":\"answer\",\"id\":" + std::to_string(id) +
               ",\"accept\":" + (accept ? "true" : "false") + "}");
}

void Client::set_auto_accept(bool enabled) {
    write_line(std::string("{\"cmd\":\"set\",\"auto_accept\":") + (enabled ? "true" : "false") + "}");
}

} // namespace lsend
