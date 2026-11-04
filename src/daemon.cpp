/**
 * @file daemon.cpp
 * @brief Enterprise Multi-Threaded Laya Decision Server Daemon with Systemd Service Support.
 * @details Features:
 *          - Linux systemd service lifecycle integration: PID file management, graceful SIGTERM/SIGINT,
 *            and atomic SIGHUP configuration reloads.
 *          - Dual-socket architecture: Per-core abstract namespace sockets (@laya_server/<cpu>)
 *            and filesystem domain socket (/run/laya/laya.sock or /tmp/laya.sock).
 *          - Enterprise XML runtime configuration (laya.xml) via embedded TinyXML2.
 *          - High-speed zero-copy dual-part IPC framing: [meta_len][JSON meta][payload_len][raw bytes].
 *          - In-memory multi-format document extraction via libarchive (.docx, .xlsx, .pptx, .odt, .zip)
 *            and poppler-cpp (.pdf) with zero disk I/O.
 *          - Google RE2 regular expression engine with XML-configurable LRU cache, idle TTL,
 *            client policy epoch delta invalidation, and SIGHUP cache flushing.
 *          - Dynamic ONNX model directory auto-discovery with role deduction and blacklist rules.
 *          - Configurable JSON response formatting via <response_template> in laya.xml.
 * 
 * @author Laya AI Team
 * @version 1.0.0
 * @date 2026-10-30
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <memory>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "laya/engine.hpp"
#include "laya/document.hpp"
#include "laya/config.hpp"
#include "laya/regex.hpp"
#include "laya/model_loader.hpp"
#include "laya/formatter.hpp"

namespace fs = std::filesystem;

static constexpr const char* DEFAULT_CONFIG_FILE = "config/laya.xml";
static constexpr const char* LOCAL_CONFIG_FILE = "laya.xml";
static constexpr const char* SYSTEM_CONFIG_FILE = "/etc/laya/laya.xml";
static constexpr const char* DEFAULT_PID_FILE = "/run/laya/laya_daemon.pid";
static constexpr const char* FALLBACK_PID_FILE = "/tmp/laya_daemon.pid";
static constexpr const char* SOCKET_PREFIX = "laya_server/";

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_reload_requested{false};
static std::unique_ptr<laya::RegexCache> g_regex_cache;
static std::string g_active_pidfile;

void handle_signal(int sig) {
    (void)sig;
    g_running.store(false);
}

void handle_sighup(int sig) {
    (void)sig;
    g_reload_requested.store(true);
}

unsigned int get_system_somaxconn() {
    unsigned int somax = 1024;
    std::ifstream f("/proc/sys/net/core/somaxconn");
    if (f.is_open()) {
        unsigned int val = 0;
        if (f >> val && val > 0) somax = val;
    }
    return somax;
}

bool write_pid_file(const std::string& path) {
    try {
        fs::path p(path);
        if (p.has_parent_path() && !fs::exists(p.parent_path())) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }
        std::ofstream ofs(path);
        if (ofs.is_open()) {
            ofs << getpid() << "\n";
            ofs.close();
            chmod(path.c_str(), 0644);
            g_active_pidfile = path;
            return true;
        }
    } catch (...) {}
    return false;
}

void remove_pid_file() {
    if (!g_active_pidfile.empty()) {
        std::error_code ec;
        fs::remove(g_active_pidfile, ec);
        g_active_pidfile.clear();
    }
}

pid_t read_pid_file(const std::string& path) {
    std::ifstream ifs(path);
    if (ifs.is_open()) {
        pid_t pid = 0;
        if (ifs >> pid && pid > 0) return pid;
    }
    return 0;
}

int do_status(const std::string& pidfile) {
    pid_t pid = read_pid_file(pidfile);
    if (pid <= 0 && pidfile == DEFAULT_PID_FILE) {
        pid = read_pid_file(FALLBACK_PID_FILE);
    }
    if (pid > 0 && kill(pid, 0) == 0) {
        std::cout << "laya-daemon is running (PID: " << pid << ")\n";
        return 0;
    }
    std::cout << "laya-daemon is stopped.\n";
    return 3; // LSB code for program is not running
}

int do_stop(const std::string& pidfile) {
    pid_t pid = read_pid_file(pidfile);
    if (pid <= 0 && pidfile == DEFAULT_PID_FILE) {
        pid = read_pid_file(FALLBACK_PID_FILE);
    }
    if (pid > 0 && kill(pid, 0) == 0) {
        std::cout << "Stopping laya-daemon (PID: " << pid << ")...\n";
        if (kill(pid, SIGTERM) == 0) {
            for (int i = 0; i < 30; ++i) {
                if (kill(pid, 0) != 0) {
                    std::cout << "laya-daemon stopped successfully.\n";
                    return 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::cout << "laya-daemon did not stop gracefully; sending SIGKILL...\n";
            kill(pid, SIGKILL);
            return 0;
        }
    }
    std::cerr << "laya-daemon is not currently running.\n";
    return 1;
}

int do_reload_signal(const std::string& pidfile) {
    pid_t pid = read_pid_file(pidfile);
    if (pid <= 0 && pidfile == DEFAULT_PID_FILE) {
        pid = read_pid_file(FALLBACK_PID_FILE);
    }
    if (pid > 0 && kill(pid, 0) == 0) {
        std::cout << "Sending SIGHUP reload signal to laya-daemon (PID: " << pid << ")...\n";
        if (kill(pid, SIGHUP) == 0) {
            std::cout << "Reload signal sent successfully.\n";
            return 0;
        }
    }
    std::cerr << "laya-daemon is not currently running.\n";
    return 1;
}

static bool read_exact(int fd, char* buf, size_t len, int timeout_ms) {
    size_t total = 0;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (total < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return false;
        ssize_t n = read(fd, buf + total, len - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

static bool send_exact(int fd, const char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, buf + total, len - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

void core_worker_loop(int core_id, unsigned int somaxconn, laya::NeuralEngine* engine) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) return;

    // 1. Abstract namespace domain socket: @laya_server/<core_id>
    int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) { close(epfd); return; }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::string socket_name = SOCKET_PREFIX + std::to_string(core_id);
    addr.sun_path[0] = '\0';
    std::memcpy(&addr.sun_path[1], socket_name.c_str(), socket_name.size());
    socklen_t addr_len = sizeof(sa_family_t) + 1 + socket_name.size();

    if (bind(listen_fd, (struct sockaddr*)&addr, addr_len) < 0) {
        std::cerr << "[Core " << core_id << "] bind error: " << strerror(errno) << std::endl;
        close(listen_fd);
        close(epfd);
        return;
    }

    listen(listen_fd, somaxconn);

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    // 2. Core 0 also hosts the filesystem Unix socket (/run/laya/laya.sock or /tmp/laya.sock)
    int fs_listen_fd = -1;
    auto config = laya::LayaConfigManager::instance().get();

    if (core_id == 0 && !config.daemon.socket_path.empty()) {
        fs::path sp(config.daemon.socket_path);
        if (sp.has_parent_path() && !fs::exists(sp.parent_path())) {
            std::error_code ec;
            fs::create_directories(sp.parent_path(), ec);
        }
        unlink(config.daemon.socket_path.c_str());
        fs_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fs_listen_fd >= 0) {
            struct sockaddr_un fs_addr;
            std::memset(&fs_addr, 0, sizeof(fs_addr));
            fs_addr.sun_family = AF_UNIX;
            strncpy(fs_addr.sun_path, config.daemon.socket_path.c_str(), sizeof(fs_addr.sun_path) - 1);
            if (bind(fs_listen_fd, (struct sockaddr*)&fs_addr, sizeof(fs_addr)) == 0) {
                listen(fs_listen_fd, somaxconn);
                chmod(config.daemon.socket_path.c_str(), 0666);
                struct epoll_event fsev;
                std::memset(&fsev, 0, sizeof(fsev));
                fsev.events = EPOLLIN | EPOLLET;
                fsev.data.fd = fs_listen_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, fs_listen_fd, &fsev);
                std::cout << "[Core 0] Filesystem Unix socket active: " << config.daemon.socket_path << std::endl;
            } else {
                close(fs_listen_fd);
                fs_listen_fd = -1;
            }
        }
    }

    std::vector<struct epoll_event> events(256);

    while (g_running.load()) {
        int n_events = epoll_wait(epfd, events.data(), events.size(), 200);
        if (n_events < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n_events; ++i) {
            int cur_fd = events[i].data.fd;

            if (cur_fd == listen_fd || (fs_listen_fd >= 0 && cur_fd == fs_listen_fd)) {
                // Accept incoming connections
                while (true) {
                    int conn_fd = accept4(cur_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (conn_fd < 0) break;

                    struct epoll_event cev;
                    std::memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
                    cev.data.fd = conn_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &cev);
                }
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                close(cur_fd);
            } else if (events[i].events & EPOLLIN) {
                int fd = cur_fd;

                // Read dual-part IPC framing:
                // [4 bytes: meta_len] [meta_len bytes: JSON metadata]
                // [4 bytes: payload_len] [payload_len bytes: raw payload]
                uint32_t meta_len_net = 0;
                if (!read_exact(fd, reinterpret_cast<char*>(&meta_len_net), 4, config.daemon.read_timeout_ms)) {
                    close(fd);
                    continue;
                }

                uint32_t meta_len = ntohl(meta_len_net);
                std::string meta_json;
                if (meta_len > 0 && meta_len < 1048576) { // Max 1MB metadata
                    meta_json.resize(meta_len);
                    if (!read_exact(fd, meta_json.data(), meta_len, config.daemon.read_timeout_ms)) {
                        close(fd);
                        continue;
                    }
                }

                uint32_t payload_len_net = 0;
                std::vector<char> payload_buf;
                if (read_exact(fd, reinterpret_cast<char*>(&payload_len_net), 4, config.daemon.read_timeout_ms)) {
                    uint32_t payload_len = ntohl(payload_len_net);
                    if (payload_len > 0 && payload_len < 268435456) { // Max 256MB document
                        payload_buf.resize(payload_len);
                        read_exact(fd, payload_buf.data(), payload_len, config.daemon.read_timeout_ms);
                    }
                }

                // Parse JSON metadata
                uint64_t epoch = 0;
                std::vector<laya::DlpPattern> patterns;
                bool is_reload_cmd = false;

                if (!meta_json.empty()) {
                    try {
                        auto j = nlohmann::json::parse(meta_json);
                        if (j.contains("cmd") && j["cmd"].is_string() && j["cmd"] == "RELOAD") {
                            is_reload_cmd = true;
                        }
                        if (j.contains("policy_epoch") && j["policy_epoch"].is_number()) {
                            epoch = j["policy_epoch"].get<uint64_t>();
                        }
                        if (j.contains("patterns") && j["patterns"].is_array()) {
                            for (const auto& item : j["patterns"]) {
                                if (item.contains("name") && item.contains("regex")) {
                                    laya::DlpPattern p;
                                    p.name = item["name"].get<std::string>();
                                    p.regex_str = item["regex"].get<std::string>();
                                    if (item.contains("verdict")) p.verdict = item["verdict"].get<std::string>();
                                    if (item.contains("score")) p.score = item["score"].get<int>();
                                    patterns.push_back(p);
                                }
                            }
                        }
                    } catch (...) {}
                }

                if (is_reload_cmd) {
                    laya::LayaConfigManager::instance().reload();
                    if (g_regex_cache) g_regex_cache->clear();
                    std::string resp = "{\"status\":\"RELOADED\",\"service\":\"laya_daemon\"}\n";
                    uint32_t resp_len = htonl(resp.size());
                    send_exact(fd, reinterpret_cast<const char*>(&resp_len), 4);
                    send_exact(fd, resp.data(), resp.size());
                } else {
                    // Extract text from payload (auto-detects PDF, Office/Zip, or plain text)
                    std::string text_to_scan;
                    if (!payload_buf.empty()) {
                        text_to_scan = laya::DocumentExtractor::extract_auto(payload_buf.data(), payload_buf.size());
                    }

                    // Merge client request patterns with daemon baseline patterns from laya.xml
                    std::vector<laya::DlpPattern> active_patterns = patterns;
                    if (config.dlp_patterns.enabled) {
                        for (const auto& cp : config.dlp_patterns.patterns) {
                            bool exists = false;
                            for (const auto& ap : active_patterns) {
                                if (ap.name == cp.name) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists) {
                                active_patterns.push_back({cp.name, cp.regex_str, cp.verdict, cp.score});
                            }
                        }
                    }

                    // Evaluate RE2 DLP regex rules with intelligent LRU cache
                    laya::DlpScanResult dlp;
                    if (g_regex_cache && !active_patterns.empty() && !text_to_scan.empty()) {
                        dlp = g_regex_cache->scan(text_to_scan, active_patterns, epoch);
                    }

                    // Run ModernBERT decision model
                    std::string verdict = "ALLOW";
                    int score = 0;
                    double latency_ms = 0.0;

                    if (engine && !text_to_scan.empty()) {
                        auto eval_res = engine->evaluate(text_to_scan);
                        verdict = eval_res.verdict;
                        score = eval_res.score;
                        latency_ms = eval_res.latency_ms;
                    }

                    // Format response using XML-configured template
                    auto cur_cfg = laya::LayaConfigManager::instance().get();
                    std::string resp = laya::JsonResponseFormatter::format(
                        verdict, score, latency_ms, dlp, cur_cfg.response_template) + "\n";

                    uint32_t resp_len = htonl(resp.size());
                    send_exact(fd, reinterpret_cast<const char*>(&resp_len), 4);
                    send_exact(fd, resp.data(), resp.size());
                }

                close(fd);
            }
        }
    }

    close(epfd);
    close(listen_fd);
    if (fs_listen_fd >= 0) {
        close(fs_listen_fd);
        unlink(config.daemon.socket_path.c_str());
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_sighup);
    signal(SIGPIPE, SIG_IGN);

    std::string config_file;
    std::string pid_file = DEFAULT_PID_FILE;
    bool daemonize = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && (i + 1 < argc)) {
            config_file = argv[++i];
        } else if ((arg == "-p" || arg == "--pidfile") && (i + 1 < argc)) {
            pid_file = argv[++i];
        } else if (arg == "-d" || arg == "--daemon") {
            daemonize = true;
        } else if (arg == "--status") {
            return do_status(pid_file);
        } else if (arg == "--stop") {
            return do_stop(pid_file);
        } else if (arg == "--reload") {
            return do_reload_signal(pid_file);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: laya_daemon [options]\n\n"
                      << "Server Options:\n"
                      << "  -c, --config <file>     Path to laya.xml configuration file\n"
                      << "  -p, --pidfile <file>    Path to PID file (default: /run/laya/laya_daemon.pid)\n"
                      << "  -d, --daemon            Detach and run as background daemon\n\n"
                      << "Service Control Commands:\n"
                      << "  --status                Check status of running laya_daemon instance\n"
                      << "  --reload                Send SIGHUP to reload XML configuration\n"
                      << "  --stop                  Gracefully stop running laya_daemon instance\n"
                      << "  -h, --help              Show this help message\n";
            return 0;
        }
    }

    // Auto-locate configuration file
    if (config_file.empty()) {
        if (fs::exists(DEFAULT_CONFIG_FILE)) {
            config_file = DEFAULT_CONFIG_FILE;
        } else if (fs::exists(LOCAL_CONFIG_FILE)) {
            config_file = LOCAL_CONFIG_FILE;
        } else if (fs::exists(SYSTEM_CONFIG_FILE)) {
            config_file = SYSTEM_CONFIG_FILE;
        } else {
            config_file = DEFAULT_CONFIG_FILE;
        }
    }

    // Attempt daemonization if requested
    if (daemonize) {
        if (daemon(1, 0) < 0) {
            std::cerr << "Error: daemon() failed: " << strerror(errno) << "\n";
            return 1;
        }
    }

    // Write PID file
    if (!write_pid_file(pid_file)) {
        if (pid_file != FALLBACK_PID_FILE) {
            write_pid_file(FALLBACK_PID_FILE);
        }
    }

    std::cout << "=== Starting Laya AI Profiling Daemon v1.0 ===" << std::endl;
    std::cout << "Configuration File:   " << config_file << std::endl;
    std::cout << "PID File:             " << (g_active_pidfile.empty() ? "(none)" : g_active_pidfile) << std::endl;

    if (!laya::LayaConfigManager::instance().load(config_file)) {
        std::cerr << "Warning: Could not load " << config_file << ", using carrier-grade defaults.\n";
    }

    auto config = laya::LayaConfigManager::instance().get();
    g_regex_cache = std::make_unique<laya::RegexCache>(
        config.regex_cache.max_entries, config.regex_cache.idle_ttl_seconds);

    // Auto-discover ONNX models
    auto discovered = laya::ModelRegistry::discover(config.models);
    std::string primary_decision_dir;
    for (const auto& m : discovered) {
        if (m.role == "DECISION") {
            primary_decision_dir = fs::path(m.full_path).parent_path().string();
            break;
        }
    }

    if (primary_decision_dir.empty()) {
        std::vector<std::string> search_dirs = {
            "model",
            "/usr/share/laya/models",
            "/usr/local/share/laya/models",
            "/usr/local/src/laya-prototype/model"
        };
        for (const auto& d : search_dirs) {
            if (fs::exists(d + "/model_quantized.onnx")) {
                primary_decision_dir = d;
                break;
            }
        }
    }

    int num_cpus = config.daemon.worker_threads > 0 ? config.daemon.worker_threads : sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus <= 0) num_cpus = 4;
    unsigned int somaxconn = get_system_somaxconn();

    std::cout << "Worker Threads:       " << num_cpus << std::endl;
    std::cout << "Filesystem Socket:    " << config.daemon.socket_path << std::endl;
    std::cout << "Regex Cache Bound:    " << config.regex_cache.max_entries << " entries (TTL: " 
              << config.regex_cache.idle_ttl_seconds << "s)\n";
    std::cout << "Baseline Patterns:    " << config.dlp_patterns.patterns.size() << " configured rules\n";
    std::cout << "Primary Decision Dir: " << primary_decision_dir << std::endl;

    std::unique_ptr<laya::NeuralEngine> engine;
    try {
        engine = std::make_unique<laya::NeuralEngine>();
        std::string model_file = primary_decision_dir + "/model_quantized.onnx";
        std::string tok_file = primary_decision_dir + "/tokenizer.json";
        if (fs::exists(model_file) && fs::exists(tok_file)) {
            if (!engine->init(model_file, tok_file, config.models.intra_op_threads)) {
                std::cerr << "Warning: Could not initialize NeuralEngine with " << model_file << "\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Engine init warning: " << ex.what() << " (daemon will serve DLP regex matching)\n";
    }

    std::vector<std::thread> workers;
    workers.reserve(num_cpus);
    for (int i = 0; i < num_cpus; ++i) {
        workers.emplace_back(core_worker_loop, i, somaxconn, engine.get());
    }

    std::cout << "Laya daemon initialized. Ready for dual-part IPC requests.\n";

    // Main thread monitors SIGHUP reload requests
    while (g_running.load()) {
        if (g_reload_requested.exchange(false)) {
            std::cout << "[laya_daemon] SIGHUP caught: reloading XML configuration...\n";
            laya::LayaConfigManager::instance().reload();
            auto reloaded_cfg = laya::LayaConfigManager::instance().get();
            if (g_regex_cache) {
                g_regex_cache->configure(reloaded_cfg.regex_cache.max_entries, reloaded_cfg.regex_cache.idle_ttl_seconds);
                g_regex_cache->clear();
            }
            std::cout << "[laya_daemon] Reload complete. Baseline patterns: " 
                      << reloaded_cfg.dlp_patterns.patterns.size() << ". Regex cache flushed.\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    remove_pid_file();
    std::cout << "Laya daemon clean shutdown.\n";
    return 0;
}
