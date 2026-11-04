/**
 * @file laya_client.cpp
 * @brief Zero-Dependency, Carrier-Grade CLI Client and Test Utility for Laya Server.
 * @details Features:
 *          - Zero hardcoding: completely configurable via CLI flags, environment, and external JSON.
 *          - Eliminates external dependencies like socat, xxd, and tail for shell scripting.
 *          - High-speed zero-copy IPC: handles 4-byte big-endian framing transparently.
 *          - Automatic connection failover: abstract socket (@laya_server/<core>) -> filesystem sockets.
 *          - Explicit socket targeting: -s /path/to/sock or -s @abstract_name.
 *          - Scripting-friendly output:
 *              * Default: full JSON string
 *              * -q, --quiet: verdict only (e.g. SAFE, SECRET_LEAK)
 *              * --field <key>: extracts specific JSON field (e.g. score, latency_ms, matched_patterns)
 *          - Pure exit codes for direct if-conditions in shell scripts:
 *              * Exit 0 = SAFE / ALLOW
 *              * Exit 1 = THREAT / DENIED
 *              * Exit 2 = Error / Connection failure
 *          - Flexible input sources: direct arguments, -f / --file <doc>, or stdin pipe (-).
 *          - Dynamic DLP: --meta <json>, --meta-file <file.json>, --patterns-file <file.json>, -p <pattern>.
 *          - Automated regression testing: --test-suite [file.json].
 *          - Daemon administration: --reload (hot reload without restart).
 *
 * @author Laya AI Team
 * @version 3.2.0
 * @date 2026-10-31
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <algorithm>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <nlohmann/json.hpp>

static constexpr const char* DEFAULT_FS_SOCKET = "/tmp/laya.sock";
static constexpr const char* ABSTRACT_SOCKET_PREFIX = "laya_server/";

/**
 * @struct ClientResponse
 * @brief Holds the parsed response received from the Laya daemon over UNIX domain socket.
 */
struct ClientResponse {
    std::string raw_json;

    /// Fail-safe default: If connection fails or socket drops before response is received,
    /// verdict safely defaults to "ERROR" (exit code 2) rather than uninitialized/safe.
    std::string verdict{"ERROR"};

    int score{0};
    double latency_ms{0.0};
    std::vector<std::string> matched_patterns;

    /// Set to true only after valid framed payload is received and parsed.
    bool success{false};
};

/**
 * @struct CliPattern
 * @brief Represents an ad-hoc or loaded DLP regular expression pattern.
 */
struct CliPattern {
    std::string name;
    std::string regex;

    /// Shorthand default: When ad-hoc patterns are supplied via CLI (-p NAME=REGEX)
    /// without explicit :VERDICT suffix, they default to "SECRET_LEAK" since regex
    /// rules primarily target credential and token leaks.
    std::string verdict{"SECRET_LEAK"};

    /// Default threat severity score for secret leaks (0-100 scale).
    int score{90};
};

bool send_all(int fd, const void* data, size_t len) {
    size_t total = 0;
    const char* ptr = static_cast<const char*>(data);
    while (total < len) {
        ssize_t n = send(fd, ptr + total, len - total, MSG_NOSIGNAL);
        if (n > 0) total += n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) usleep(500);
        else return false;
    }
    return true;
}

bool recv_all(int fd, void* dest, size_t len) {
    size_t total = 0;
    char* ptr = static_cast<char*>(dest);
    while (total < len) {
        ssize_t n = recv(fd, ptr + total, len - total, 0);
        if (n > 0) total += n;
        else if (n == 0) return false;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) usleep(500);
        else return false;
    }
    return true;
}

/**
 * @brief Connects to Laya daemon via custom socket or auto-discovering abstract/filesystem paths.
 */
int connect_daemon(const std::string& custom_socket = "", int core_id = 0) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;

    // 1. Explicit custom socket path
    if (!custom_socket.empty()) {
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (custom_socket[0] == '@') {
            // Abstract namespace socket
            addr.sun_path[0] = '\0';
            strncpy(addr.sun_path + 1, custom_socket.c_str() + 1, sizeof(addr.sun_path) - 2);
            socklen_t addr_len = sizeof(sa_family_t) + custom_socket.size();
            if (connect(fd, (struct sockaddr*)&addr, addr_len) == 0) return fd;
        } else {
            // Filesystem socket
            strncpy(addr.sun_path, custom_socket.c_str(), sizeof(addr.sun_path) - 1);
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
        }
        close(fd);
        return -1;
    }

    // 2. Default: Try abstract namespace domain socket (@laya_server/<core_id>)
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    int written = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 2, "%s%d", ABSTRACT_SOCKET_PREFIX, core_id);
    socklen_t addr_len = sizeof(sa_family_t) + 1 + written;

    if (connect(fd, (struct sockaddr*)&addr, addr_len) == 0) {
        return fd;
    }

    // 3. Fallback: filesystem domain sockets (/run/laya/laya.sock or /tmp/laya.sock)
    for (const char* fs_sock : {"/run/laya/laya.sock", "/tmp/laya.sock"}) {
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, fs_sock, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            return fd;
        }
    }

    close(fd);
    return -1;
}

/**
 * @brief Sends dual-part framed binary request to Laya daemon and receives JSON response.
 */
ClientResponse send_dual_part_request(const std::string& metadata_json, const void* payload, size_t payload_len, const std::string& custom_socket = "") {
    ClientResponse resp;
    int fd = connect_daemon(custom_socket);
    if (fd < 0) {
        std::cerr << "Error: Cannot connect to Laya daemon socket (neither abstract nor " 
                  << DEFAULT_FS_SOCKET << "). Is laya_daemon running?\n";
        return resp;
    }

    // 1. Send meta_len (4 bytes, network byte order)
    uint32_t meta_len = htonl(metadata_json.size());
    if (!send_all(fd, &meta_len, 4)) { close(fd); return resp; }

    // 2. Send metadata JSON string
    if (!metadata_json.empty()) {
        if (!send_all(fd, metadata_json.data(), metadata_json.size())) { close(fd); return resp; }
    }

    // 3. Send payload_len (4 bytes, network byte order)
    uint32_t payload_len_net = htonl(payload_len);
    if (!send_all(fd, &payload_len_net, 4)) { close(fd); return resp; }

    // 4. Send payload raw bytes
    if (payload_len > 0 && payload != nullptr) {
        if (!send_all(fd, payload, payload_len)) { close(fd); return resp; }
    }

    // 5. Read response: 4-byte length prefix
    uint32_t resp_len_net = 0;
    if (!recv_all(fd, &resp_len_net, 4)) { close(fd); return resp; }

    uint32_t resp_len = ntohl(resp_len_net);
    if (resp_len > 0 && resp_len < 16777216) {
        resp.raw_json.resize(resp_len);
        if (recv_all(fd, resp.raw_json.data(), resp_len)) {
            resp.success = true;
        }
    }

    close(fd);

    // Parse JSON fields
    if (resp.success) {
        try {
            auto j = nlohmann::json::parse(resp.raw_json);
            if (j.contains("verdict") && j["verdict"].is_string()) {
                resp.verdict = j["verdict"].get<std::string>();
            }
            if (j.contains("score") && j["score"].is_number()) {
                resp.score = j["score"].get<int>();
            }
            if (j.contains("latency_ms") && j["latency_ms"].is_number()) {
                resp.latency_ms = j["latency_ms"].get<double>();
            }
            if (j.contains("matched_patterns") && j["matched_patterns"].is_array()) {
                for (const auto& item : j["matched_patterns"]) {
                    if (item.is_string()) {
                        resp.matched_patterns.push_back(item.get<std::string>());
                    }
                }
            }
        } catch (...) {
            // Fallback manual field extraction if not valid JSON
            auto get_field = [&](const std::string& key) -> std::string {
                size_t pos = resp.raw_json.find("\"" + key + "\"");
                if (pos == std::string::npos) return "";
                pos += key.size() + 3;
                while (pos < resp.raw_json.size() && (resp.raw_json[pos] == ' ' || resp.raw_json[pos] == '\"')) pos++;
                size_t end = resp.raw_json.find_first_of("\",}\r\n", pos);
                if (end == std::string::npos) return "";
                return resp.raw_json.substr(pos, end - pos);
            };
            resp.verdict = get_field("verdict");
        }
    }

    return resp;
}

std::vector<CliPattern> load_patterns_from_json(const std::string& path) {
    std::vector<CliPattern> result;
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return result;
    }
    try {
        nlohmann::json j;
        ifs >> j;
        nlohmann::json pat_array;
        if (j.is_array()) {
            pat_array = j;
        } else if (j.contains("patterns") && j["patterns"].is_array()) {
            pat_array = j["patterns"];
        }
        for (const auto& item : pat_array) {
            if (item.contains("name") && item.contains("regex")) {
                CliPattern p;
                p.name = item["name"].get<std::string>();
                p.regex = item["regex"].get<std::string>();
                if (item.contains("verdict") && item["verdict"].is_string()) {
                    p.verdict = item["verdict"].get<std::string>();
                }
                if (item.contains("score") && item["score"].is_number()) {
                    p.score = item["score"].get<int>();
                }
                result.push_back(std::move(p));
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Warning: Failed to parse patterns from " << path << ": " << ex.what() << "\n";
    }
    return result;
}

/**
 * @brief Parses an ad-hoc pattern CLI argument: NAME=REGEX[:VERDICT[:SCORE]]
 * @example
 *   Shorthand: "AWS_KEY=AKIA[0-9A-Z]{16}" -> verdict="SECRET_LEAK", score=90
 *   Explicit:  "EXPLOIT_CVE=CVE-[0-9]+:EXPLOIT:95" -> verdict="EXPLOIT", score=95
 */
bool parse_cli_pattern(const std::string& arg, CliPattern& out) {
    size_t eq = arg.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    out.name = arg.substr(0, eq);
    std::string rest = arg.substr(eq + 1);

    // Default values if optional :VERDICT or :SCORE suffixes are omitted
    out.verdict = "SECRET_LEAK";
    out.score = 90;

    size_t colon1 = rest.rfind(':');
    if (colon1 != std::string::npos) {
        std::string part2 = rest.substr(colon1 + 1);
        bool is_num = !part2.empty() && std::all_of(part2.begin(), part2.end(), ::isdigit);
        if (is_num) {
            out.score = std::stoi(part2);
            std::string before_score = rest.substr(0, colon1);
            size_t colon0 = before_score.rfind(':');
            if (colon0 != std::string::npos) {
                out.verdict = before_score.substr(colon0 + 1);
                out.regex = before_score.substr(0, colon0);
            } else {
                out.regex = before_score;
            }
        } else {
            out.verdict = part2;
            out.regex = rest.substr(0, colon1);
        }
    } else {
        out.regex = rest;
    }
    return !out.name.empty() && !out.regex.empty();
}

std::string find_default_patterns_file() {
    for (const char* path : {"/etc/laya/patterns.json", "config/patterns.json", "./patterns.json"}) {
        if (access(path, R_OK) == 0) {
            return path;
        }
    }
    return "";
}

void print_help() {
    std::cout << "Laya AI Multimodal Client Utility v3.2\n"
              << "Usage: laya_client [options] [text]\n\n"
              << "Input Options:\n"
              << "  <text>                      Direct text prompt to evaluate\n"
              << "  -f, --file <path>           Evaluate document file (PDF, DOCX, XLSX, TXT, images)\n"
              << "  -                           Stream input from stdin (pipe mode)\n\n"
              << "Connection Options:\n"
              << "  -s, --socket <path|@name>   Target socket path (/run/laya/laya.sock or @laya_server/0)\n\n"
              << "Metadata & Rule Options:\n"
              << "  -m, --meta <json>           Pass explicit JSON metadata string\n"
              << "  -M, --meta-file <path.json> Read metadata JSON directly from file\n"
              << "  --patterns-file <path.json> Load DLP regex patterns from external JSON file\n"
              << "  -p, --pattern <spec>        Pass ad-hoc pattern: NAME=REGEX[:VERDICT[:SCORE]] (can repeat)\n\n"
              << "Output Formatting (Shell-Friendly):\n"
              << "  -q, --quiet                 Print ONLY the final verdict (SAFE, SECRET_LEAK, MALICIOUS, etc.)\n"
              << "  --field <key>               Extract specific JSON field (e.g. score, latency_ms, matched_patterns)\n\n"
              << "Automated Testing & Control:\n"
              << "  --test-suite [suite.json]   Run automated regression test suite (default: /etc/laya/test_suite.json)\n"
              << "  --reload                    Signal daemon to hot-reload configuration and clear cache\n"
              << "  -h, --help                  Show this help guide\n\n"
              << "Exit Codes:\n"
              << "  0 = SAFE / ALLOW\n"
              << "  1 = THREAT / DENIED\n"
              << "  2 = Error / Connection Failure\n";
}

int run_test_suite(const std::string& suite_file_arg, const std::string& patterns_file_arg, const std::string& socket_path) {
    std::string suite_path = suite_file_arg;
    if (suite_path.empty()) {
        for (const char* p : {"/etc/laya/test_suite.json", "config/test_suite.json", "./test_suite.json"}) {
            if (access(p, R_OK) == 0) {
                suite_path = p;
                break;
            }
        }
    }

    if (suite_path.empty() || access(suite_path.c_str(), R_OK) != 0) {
        std::cerr << "Error: Test suite file not found. Provide path via --test-suite <file.json> "
                  << "or install /etc/laya/test_suite.json.\n";
        return 2;
    }

    std::ifstream ifs(suite_path);
    if (!ifs.is_open()) {
        std::cerr << "Error: Could not open test suite file: " << suite_path << "\n";
        return 2;
    }

    nlohmann::json root_json;
    try {
        ifs >> root_json;
    } catch (const std::exception& ex) {
        std::cerr << "Error: Failed to parse test suite JSON (" << suite_path << "): " << ex.what() << "\n";
        return 2;
    }

    if (!root_json.contains("test_cases") || !root_json["test_cases"].is_array()) {
        std::cerr << "Error: Invalid test suite format: missing 'test_cases' array in " << suite_path << "\n";
        return 2;
    }

    // Load patterns for test suite (from patterns file if available)
    std::string pat_file = patterns_file_arg;
    if (pat_file.empty()) pat_file = find_default_patterns_file();
    std::vector<CliPattern> suite_patterns;
    if (!pat_file.empty()) {
        suite_patterns = load_patterns_from_json(pat_file);
    }

    // Build metadata JSON
    nlohmann::json meta_obj;
    meta_obj["policy_epoch"] = 1;
    nlohmann::json pat_arr = nlohmann::json::array();
    for (const auto& pat : suite_patterns) {
        nlohmann::json pj;
        pj["name"] = pat.name;
        pj["regex"] = pat.regex;
        pj["verdict"] = pat.verdict;
        pj["score"] = pat.score;
        pat_arr.push_back(pj);
    }
    meta_obj["patterns"] = pat_arr;
    std::string meta_json = meta_obj.dump();

    std::cout << "=== Running Laya Automated Regression Test Suite ===\n";
    std::cout << "Suite file:    " << suite_path << "\n";
    std::cout << "Active rules:  " << suite_patterns.size() << " patterns configured\n\n";

    const auto& cases = root_json["test_cases"];
    int passed = 0;
    int total = cases.size();

    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& tc = cases[i];
        std::string name = tc.value("name", "Test " + std::to_string(i + 1));
        std::string prompt = tc.value("prompt", "");
        std::string expected = tc.value("expected_verdict", "SAFE");

        auto resp = send_dual_part_request(meta_json, prompt.data(), prompt.size(), socket_path);
        bool ok = resp.success && (resp.verdict == expected || resp.verdict.find(expected) != std::string::npos);
        if (ok) passed++;

        std::cout << "Test " << std::setw(2) << (i + 1) << ": " << std::left << std::setw(38) << name
                  << " Expected: " << std::setw(16) << expected
                  << " Got: " << std::setw(16) << (resp.success ? resp.verdict : "ERROR")
                  << " [" << (ok ? "PASS" : "FAIL") << "]\n";
    }

    std::cout << "\nResults: " << passed << "/" << total << " passed.\n";
    return (passed == total) ? 0 : 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    bool quiet = false;
    std::string output_field;
    std::string socket_path;
    std::string explicit_meta;
    std::string explicit_meta_file;
    std::string file_path;
    std::string prompt_text;
    std::string patterns_file;
    std::vector<CliPattern> cli_patterns;
    bool is_pipe = false;
    bool is_reload = false;
    bool is_test_suite = false;
    std::string test_suite_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg == "--field" && i + 1 < argc) {
            output_field = argv[++i];
        } else if ((arg == "-s" || arg == "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((arg == "-m" || arg == "--meta") && i + 1 < argc) {
            explicit_meta = argv[++i];
        } else if ((arg == "-M" || arg == "--meta-file") && i + 1 < argc) {
            explicit_meta_file = argv[++i];
        } else if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
            file_path = argv[++i];
        } else if (arg == "-") {
            is_pipe = true;
        } else if (arg == "--reload") {
            is_reload = true;
        } else if (arg == "--test-suite") {
            is_test_suite = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                test_suite_path = argv[++i];
            }
        } else if ((arg == "-p" || arg == "--pattern") && i + 1 < argc) {
            CliPattern cp;
            if (parse_cli_pattern(argv[++i], cp)) {
                cli_patterns.push_back(std::move(cp));
            } else {
                std::cerr << "Warning: Invalid pattern specification: " << argv[i] << "\n";
            }
        } else if (arg == "--patterns-file" && i + 1 < argc) {
            patterns_file = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (prompt_text.empty() && arg[0] != '-') {
            prompt_text = arg;
        }
    }

    if (is_test_suite) {
        return run_test_suite(test_suite_path, patterns_file, socket_path);
    }

    if (is_reload) {
        std::string meta = "{\"cmd\":\"RELOAD\"}";
        auto resp = send_dual_part_request(meta, nullptr, 0, socket_path);
        if (resp.success) {
            std::cout << "Laya daemon successfully reloaded configuration: " << resp.raw_json << "\n";
            return 0;
        } else {
            std::cerr << "Failed to reload Laya daemon.\n";
            return 2;
        }
    }

    std::vector<char> payload_bytes;

    if (is_pipe) {
        // Read stdin until EOF
        char buf[8192];
        while (std::cin.read(buf, sizeof(buf)) || std::cin.gcount() > 0) {
            payload_bytes.insert(payload_bytes.end(), buf, buf + std::cin.gcount());
        }
    } else if (!file_path.empty()) {
        std::ifstream ifs(file_path, std::ios::binary);
        if (!ifs.is_open()) {
            std::cerr << "Error: Could not open file '" << file_path << "'\n";
            return 2;
        }
        payload_bytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    } else if (!prompt_text.empty()) {
        payload_bytes.assign(prompt_text.begin(), prompt_text.end());
    } else {
        print_help();
        return 0;
    }

    // Determine metadata JSON:
    std::string meta;
    if (!explicit_meta.empty()) {
        meta = explicit_meta;
    } else if (!explicit_meta_file.empty()) {
        std::ifstream ifs(explicit_meta_file);
        if (!ifs.is_open()) {
            std::cerr << "Error: Could not open metadata file '" << explicit_meta_file << "'\n";
            return 2;
        }
        meta.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    } else {
        // Build metadata from patterns-file + ad-hoc patterns
        std::vector<CliPattern> active_patterns;
        if (!patterns_file.empty()) {
            active_patterns = load_patterns_from_json(patterns_file);
        } else {
            std::string def_path = find_default_patterns_file();
            if (!def_path.empty()) {
                active_patterns = load_patterns_from_json(def_path);
            }
        }
        for (const auto& cp : cli_patterns) {
            active_patterns.push_back(cp);
        }

        nlohmann::json meta_obj;
        meta_obj["policy_epoch"] = 1;
        nlohmann::json pat_arr = nlohmann::json::array();
        for (const auto& pat : active_patterns) {
            nlohmann::json pj;
            pj["name"] = pat.name;
            pj["regex"] = pat.regex;
            pj["verdict"] = pat.verdict;
            pj["score"] = pat.score;
            pat_arr.push_back(pj);
        }
        meta_obj["patterns"] = pat_arr;
        meta = meta_obj.dump();
    }

    auto resp = send_dual_part_request(meta, payload_bytes.data(), payload_bytes.size(), socket_path);

    if (!resp.success) {
        if (!quiet) std::cerr << "Error: Evaluation request failed.\n";
        return 2;
    }

    // Output formatting
    if (!output_field.empty()) {
        try {
            auto j = nlohmann::json::parse(resp.raw_json);
            if (j.contains(output_field)) {
                if (j[output_field].is_string()) {
                    std::cout << j[output_field].get<std::string>() << "\n";
                } else if (j[output_field].is_array()) {
                    bool first = true;
                    for (const auto& el : j[output_field]) {
                        if (!first) std::cout << ",";
                        first = false;
                        if (el.is_string()) std::cout << el.get<std::string>();
                        else std::cout << el.dump();
                    }
                    std::cout << "\n";
                } else {
                    std::cout << j[output_field].dump() << "\n";
                }
            } else {
                std::cout << "\n";
            }
        } catch (...) {
            std::cout << "\n";
        }
    } else if (quiet) {
        std::cout << resp.verdict << "\n";
    } else {
        std::cout << resp.raw_json << "\n";
    }

    // Exit code: 0 = ALLOW/SAFE, 1 = DENIED/THREAT
    if (resp.verdict == "ALLOW" || resp.verdict == "SAFE" || resp.score < 50) {
        return 0;
    }
    return 1;
}
