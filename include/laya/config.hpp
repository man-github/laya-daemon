/**
 * @file laya_config.hpp
 * @brief Enterprise XML configuration parser and manager for Laya AI daemon.
 *
 * Utilizes embedded TinyXML2 to parse runtime configuration (laya.xml).
 * Provides thread-safe read access and SIGHUP hot-reload capabilities.
 */

#pragma once

#include <string>
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <memory>
#include <algorithm>
#include <iostream>
#include <sstream>
#include "tinyxml2.h"

namespace laya {

/**
 * @struct DaemonConfig
 * @brief Network socket and worker concurrency configuration.
 */
struct DaemonConfig {
    bool enabled = true;
    std::string socket_path = "/tmp/laya.sock";
    int worker_threads = 4;
    int max_connections = 1024;
    int read_timeout_ms = 5000;
    std::string socket_permissions = "0660";
};

/**
 * @struct RegexCacheConfig
 * @brief RE2 compiled regex cache bounds and eviction settings.
 */
struct RegexCacheConfig {
    bool enabled = true;
    size_t max_entries = 512;
    int idle_ttl_seconds = 900;
};

/**
 * @struct DirectoryEntry
 * @brief Model auto-discovery directory specification.
 */
struct DirectoryEntry {
    int id = 0;
    bool enabled = true;
    std::string comment;
    std::string path;
    bool recursive = false;
};

/**
 * @struct ModelBindingEntry
 * @brief Explicit model role binding and companion asset mapping.
 */
struct ModelBindingEntry {
    int id = 0;
    bool enabled = true;
    std::string comment;
    std::string filename;
    std::string role;           ///< "DECISION", "OCR_DET", "OCR_REC"
    std::string companion_path; ///< Path to tokenizer.json or dictionary keys txt
};

/**
 * @struct ExcludeEntry
 * @brief Model blacklist wildcard patterns.
 */
struct ExcludeEntry {
    int id = 0;
    bool enabled = true;
    std::string comment;
    std::string pattern;        ///< Comma-separated wildcards (e.g. "*_test.onnx,test_*")
};

/**
 * @struct ModelsConfig
 * @brief Aggregation of model directories, bindings, and exclusions.
 */
struct ModelsConfig {
    bool enabled = true;
    int intra_op_threads = 1;
    int inter_op_threads = 1;
    std::vector<DirectoryEntry> directories;
    std::vector<ModelBindingEntry> bindings;
    std::vector<ExcludeEntry> exclusions;
};

/**
 * @struct ResponseTemplateConfig
 * @brief Configurable JSON response fields and key remapping.
 */
struct ResponseTemplateConfig {
    bool enabled = true;
    std::string format = "JSON";
    bool include_verdict = true;
    bool include_score = true;
    bool include_latency = true;
    bool include_matched_patterns = true;
    bool include_tokens = false;
    std::string verdict_key = "verdict";
    std::string score_key = "score";
    std::string latency_key = "latency_ms";
    std::string matched_patterns_key = "matched_patterns";
};

/**
 * @struct LayaConfig
 * @brief Root configuration tree parsed from laya.xml.
 */
struct LayaConfig {
    DaemonConfig daemon;
    RegexCacheConfig regex_cache;
    ModelsConfig models;
    ResponseTemplateConfig response_template;
    std::string loaded_path;
};

/**
 * @class LayaConfigManager
 * @brief Thread-safe configuration manager supporting runtime SIGHUP reload.
 */
class LayaConfigManager {
public:
    static LayaConfigManager& instance() {
        static LayaConfigManager mgr;
        return mgr;
    }

    /**
     * @brief Load configuration from an XML file path.
     * @param filepath Path to laya.xml.
     * @return true on success, false on parsing or filesystem error.
     */
    bool load(const std::string& filepath) {
        LayaConfig new_cfg;
        if (!parse_file(filepath, new_cfg)) {
            return false;
        }

        std::unique_lock<std::shared_mutex> lock(mutex_);
        config_ = std::move(new_cfg);
        config_path_ = filepath;
        return true;
    }

    /**
     * @brief Reload configuration from the previously loaded path.
     * @return true on success, false on failure (previous config preserved).
     */
    bool reload() {
        std::string path;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            path = config_path_;
        }

        if (path.empty()) {
            std::cerr << "[laya_config] Cannot reload: no previous configuration path set.\n";
            return false;
        }

        LayaConfig new_cfg;
        if (!parse_file(path, new_cfg)) {
            std::cerr << "[laya_config] Reload failed parsing " << path << ". Preserving existing configuration.\n";
            return false;
        }

        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            config_ = std::move(new_cfg);
            std::cout << "[laya_config] Successfully reloaded configuration from " << path << "\n";
        }
        return true;
    }

    /**
     * @brief Retrieve a snapshot copy of current configuration.
     * @note Thread-Safe: Multiple readers can call concurrently.
     */
    LayaConfig get() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return config_;
    }

private:
    LayaConfigManager() = default;
    ~LayaConfigManager() = default;
    LayaConfigManager(const LayaConfigManager&) = delete;
    LayaConfigManager& operator=(const LayaConfigManager&) = delete;

    mutable std::shared_mutex mutex_;
    LayaConfig config_;
    std::string config_path_;

    static bool parse_bool(const char* val, bool default_val = false) {
        if (!val) return default_val;
        std::string s(val);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
        return default_val;
    }

    static std::string get_child_text(const tinyxml2::XMLElement* parent, const char* name, const std::string& default_val = "") {
        if (!parent) return default_val;
        const tinyxml2::XMLElement* elem = parent->FirstChildElement(name);
        if (elem && elem->GetText()) {
            return elem->GetText();
        }
        return default_val;
    }

    static bool get_child_bool(const tinyxml2::XMLElement* parent, const char* name, bool default_val = false) {
        if (!parent) return default_val;
        const tinyxml2::XMLElement* elem = parent->FirstChildElement(name);
        if (elem && elem->GetText()) {
            return parse_bool(elem->GetText(), default_val);
        }
        return default_val;
    }

    static int get_child_int(const tinyxml2::XMLElement* parent, const char* name, int default_val = 0) {
        if (!parent) return default_val;
        const tinyxml2::XMLElement* elem = parent->FirstChildElement(name);
        if (elem && elem->GetText()) {
            try {
                return std::stoi(elem->GetText());
            } catch (...) {
                return default_val;
            }
        }
        return default_val;
    }

    bool parse_file(const std::string& filepath, LayaConfig& cfg) {
        tinyxml2::XMLDocument doc;
        tinyxml2::XMLError err = doc.LoadFile(filepath.c_str());
        if (err != tinyxml2::XML_SUCCESS) {
            std::cerr << "[laya_config] TinyXML2 error loading '" << filepath << "': " 
                      << doc.ErrorStr() << "\n";
            return false;
        }

        const tinyxml2::XMLElement* root = doc.RootElement();
        if (!root || (std::string(root->Name()) != "laya" && std::string(root->Name()) != "section")) {
            std::cerr << "[laya_config] Invalid root element in '" << filepath << "', expected <laya>\n";
            return false;
        }

        cfg.loaded_path = filepath;

        // 1. <daemon>
        const tinyxml2::XMLElement* daemon_elem = root->FirstChildElement("daemon");
        if (daemon_elem) {
            cfg.daemon.enabled = get_child_bool(daemon_elem, "enabled", true);
            cfg.daemon.socket_path = get_child_text(daemon_elem, "socket_path", "/tmp/laya.sock");
            cfg.daemon.worker_threads = get_child_int(daemon_elem, "worker_threads", 4);
            cfg.daemon.max_connections = get_child_int(daemon_elem, "max_connections", 1024);
            cfg.daemon.read_timeout_ms = get_child_int(daemon_elem, "read_timeout_ms", 5000);
            cfg.daemon.socket_permissions = get_child_text(daemon_elem, "socket_permissions", "0660");
        }

        // 2. <regex_cache>
        const tinyxml2::XMLElement* rc_elem = root->FirstChildElement("regex_cache");
        if (rc_elem) {
            cfg.regex_cache.enabled = get_child_bool(rc_elem, "enabled", true);
            cfg.regex_cache.max_entries = static_cast<size_t>(get_child_int(rc_elem, "max_entries", 512));
            cfg.regex_cache.idle_ttl_seconds = get_child_int(rc_elem, "idle_ttl_seconds", 900);
        }

        // 3. <models>
        const tinyxml2::XMLElement* models_elem = root->FirstChildElement("models");
        if (models_elem) {
            cfg.models.enabled = get_child_bool(models_elem, "enabled", true);
            cfg.models.intra_op_threads = get_child_int(models_elem, "intra_op_threads", 1);
            cfg.models.inter_op_threads = get_child_int(models_elem, "inter_op_threads", 1);

            // Parse <directory id="N">
            for (const tinyxml2::XMLElement* d = models_elem->FirstChildElement("directory"); d; d = d->NextSiblingElement("directory")) {
                DirectoryEntry entry;
                entry.id = d->IntAttribute("id", 0);
                entry.enabled = get_child_bool(d, "enabled", true);
                entry.comment = get_child_text(d, "comment");
                entry.path = get_child_text(d, "path");
                entry.recursive = get_child_bool(d, "recursive", false);
                if (!entry.path.empty()) {
                    cfg.models.directories.push_back(entry);
                }
            }

            // Parse <model id="N">
            for (const tinyxml2::XMLElement* m = models_elem->FirstChildElement("model"); m; m = m->NextSiblingElement("model")) {
                ModelBindingEntry entry;
                entry.id = m->IntAttribute("id", 0);
                entry.enabled = get_child_bool(m, "enabled", true);
                entry.comment = get_child_text(m, "comment");
                entry.filename = get_child_text(m, "filename");
                entry.role = get_child_text(m, "role", "DECISION");
                entry.companion_path = get_child_text(m, "companion_path");
                if (!entry.filename.empty()) {
                    cfg.models.bindings.push_back(entry);
                }
            }

            // Parse <exclude id="N">
            for (const tinyxml2::XMLElement* e = models_elem->FirstChildElement("exclude"); e; e = e->NextSiblingElement("exclude")) {
                ExcludeEntry entry;
                entry.id = e->IntAttribute("id", 0);
                entry.enabled = get_child_bool(e, "enabled", true);
                entry.comment = get_child_text(e, "comment");
                entry.pattern = get_child_text(e, "pattern");
                if (!entry.pattern.empty()) {
                    cfg.models.exclusions.push_back(entry);
                }
            }
        }

        // 4. <response_template>
        const tinyxml2::XMLElement* resp_elem = root->FirstChildElement("response_template");
        if (resp_elem) {
            cfg.response_template.enabled = get_child_bool(resp_elem, "enabled", true);
            cfg.response_template.format = get_child_text(resp_elem, "format", "JSON");
            cfg.response_template.include_verdict = get_child_bool(resp_elem, "include_verdict", true);
            cfg.response_template.include_score = get_child_bool(resp_elem, "include_score", true);
            cfg.response_template.include_latency = get_child_bool(resp_elem, "include_latency", true);
            cfg.response_template.include_matched_patterns = get_child_bool(resp_elem, "include_matched_patterns", true);
            cfg.response_template.include_tokens = get_child_bool(resp_elem, "include_tokens", false);
            cfg.response_template.verdict_key = get_child_text(resp_elem, "verdict_key", "verdict");
            cfg.response_template.score_key = get_child_text(resp_elem, "score_key", "score");
            cfg.response_template.latency_key = get_child_text(resp_elem, "latency_key", "latency_ms");
            cfg.response_template.matched_patterns_key = get_child_text(resp_elem, "matched_patterns_key", "matched_patterns");
        }

        return true;
    }
};

} // namespace laya
