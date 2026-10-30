/**
 * @file laya_model_loader.hpp
 * @brief Dynamic ONNX model directory auto-discovery, role deduction, and blacklist filtering.
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <fnmatch.h>
#include <iostream>
#include <sstream>
#include "config.hpp"

namespace laya {

/**
 * @struct DiscoveredModel
 * @brief Discovered ONNX model instance with resolved pipeline role and companion assets.
 */
struct DiscoveredModel {
    std::string filename;
    std::string full_path;
    std::string role;           ///< "DECISION", "OCR_DET", "OCR_REC"
    std::string companion_path; ///< tokenizer.json or dictionary keys txt
};

/**
 * @class ModelRegistry
 * @brief Scans configured directories, applies exclusion filters, and resolves roles.
 */
class ModelRegistry {
public:
    static std::vector<DiscoveredModel> discover(const ModelsConfig& config) {
        std::vector<DiscoveredModel> results;

        if (!config.enabled) {
            std::cout << "[laya_model_loader] Model loading is disabled in configuration.\n";
            return results;
        }

        // Parse exclusion patterns
        std::vector<std::string> exclude_patterns;
        for (const auto& exc : config.exclusions) {
            if (!exc.enabled) continue;
            std::stringstream ss(exc.pattern);
            std::string item;
            while (std::getline(ss, item, ',')) {
                // Trim whitespace
                size_t first = item.find_first_not_of(" \t\r\n");
                size_t last = item.find_last_not_of(" \t\r\n");
                if (first != std::string::npos && last != std::string::npos) {
                    exclude_patterns.push_back(item.substr(first, last - first + 1));
                }
            }
        }

        // Scan directories
        for (const auto& dir_entry : config.directories) {
            if (!dir_entry.enabled) continue;

            std::filesystem::path dir_path(dir_entry.path);
            if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
                std::cerr << "[laya_model_loader] Directory not found: " << dir_entry.path << "\n";
                continue;
            }

            auto scan_dir = [&](const std::filesystem::path& p) {
                for (const auto& entry : std::filesystem::directory_iterator(p)) {
                    if (!entry.is_regular_file()) continue;

                    std::string fname = entry.path().filename().string();
                    if (entry.path().extension() != ".onnx") continue;

                    // Check exclusions
                    bool excluded = false;
                    for (const auto& pat : exclude_patterns) {
                        if (fnmatch(pat.c_str(), fname.c_str(), 0) == 0) {
                            std::cout << "[laya_model_loader] Excluding model '" << fname 
                                      << "' matching blacklist pattern '" << pat << "'\n";
                            excluded = true;
                            break;
                        }
                    }
                    if (excluded) continue;

                    DiscoveredModel m;
                    m.filename = fname;
                    m.full_path = entry.path().string();

                    // Check explicit binding overrides
                    bool bound = false;
                    for (const auto& b : config.bindings) {
                        if (b.enabled && b.filename == fname) {
                            m.role = b.role;
                            m.companion_path = b.companion_path;
                            bound = true;
                            break;
                        }
                    }

                    // Auto-deduce role if not explicitly bound
                    if (!bound) {
                        if (fname.find("det.onnx") != std::string::npos || fname.find("detector") != std::string::npos) {
                            m.role = "OCR_DET";
                        } else if (fname.find("rec.onnx") != std::string::npos || fname.find("recognizer") != std::string::npos) {
                            m.role = "OCR_REC";
                            // Look for dictionary keys in same directory
                            std::filesystem::path keys = entry.path().parent_path() / "ch_PP-OCR_keys_v1.txt";
                            if (std::filesystem::exists(keys)) {
                                m.companion_path = keys.string();
                            }
                        } else {
                            m.role = "DECISION";
                            // Look for tokenizer.json in same directory
                            std::filesystem::path tok = entry.path().parent_path() / "tokenizer.json";
                            if (std::filesystem::exists(tok)) {
                                m.companion_path = tok.string();
                            }
                        }
                    }

                    std::cout << "[laya_model_loader] Discovered model: " << m.filename 
                              << " [Role: " << m.role << "] Companion: " 
                              << (m.companion_path.empty() ? "(none)" : m.companion_path) << "\n";
                    results.push_back(m);
                }
            };

            scan_dir(dir_path);
        }

        return results;
    }
};

} // namespace laya
