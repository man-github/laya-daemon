/**
 * @file laya_formatter.hpp
 * @brief Dynamic JSON response formatter configurable via XML template.
 */

#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include "config.hpp"
#include "regex.hpp"

namespace laya {

/**
 * @class JsonResponseFormatter
 * @brief Constructs JSON verdicts honoring the XML <response_template> configuration.
 */
class JsonResponseFormatter {
public:
    static std::string format(
        const std::string& verdict,
        int score,
        double latency_ms,
        const DlpScanResult& dlp,
        const ResponseTemplateConfig& cfg)
    {
        std::ostringstream ss;
        ss << "{";

        bool first = true;
        auto add_field = [&](const std::string& key, const std::string& val, bool is_str = true) {
            if (!first) ss << ", ";
            first = false;
            ss << "\"" << key << "\": ";
            if (is_str) ss << "\"" << val << "\"";
            else ss << val;
        };

        // Verdict
        if (cfg.include_verdict) {
            std::string final_verdict = dlp.matched ? dlp.primary_verdict : verdict;
            add_field(cfg.verdict_key, final_verdict, true);
        }

        // Score
        if (cfg.include_score) {
            int final_score = dlp.matched ? std::max(score, dlp.max_score) : score;
            add_field(cfg.score_key, std::to_string(final_score), false);
        }

        // Latency
        if (cfg.include_latency) {
            std::ostringstream lat_ss;
            lat_ss << std::fixed << std::setprecision(2) << latency_ms;
            add_field(cfg.latency_key, lat_ss.str(), false);
        }

        // Matched Patterns
        if (cfg.include_matched_patterns) {
            if (!first) ss << ", ";
            first = false;
            ss << "\"" << cfg.matched_patterns_key << "\": [";
            for (size_t i = 0; i < dlp.matched_pattern_names.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << "\"" << dlp.matched_pattern_names[i] << "\"";
            }
            ss << "]";
        }

        ss << "}";
        return ss.str();
    }
};

} // namespace laya
