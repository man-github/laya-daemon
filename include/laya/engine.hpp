/**
 * @file laya_engine.hpp
 * @brief Thread-Safe Neural Inference Engine wrapping ONNX Runtime for Laya Server.
 * @details Executes forward passes with dynamic sequence lengths on CPU using INT8
 *          quantized DeBERTa-v3 model. Combines microsecond heuristic DLP scanning
 *          with deep neural semantic injection classification.
 *
 * @author Laya AI Team
 * @version 1.0.0
 * @date 2026-10-29
 */

#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <onnxruntime_cxx_api.h>
#include "tokenizer.hpp"

namespace laya {

/**
 * @struct InferenceVerdict
 * @brief Complete classification result returned to caller.
 */
struct InferenceVerdict {
    std::string decision;       ///< Action category: "choice"
    std::string verdict;        ///< Classification: "SAFE", "PROMPT_INJECTION", "SECRET_LEAK"
    double confidence;          ///< Probability (0.0 to 1.0)
    int score;                  ///< Integer severity score (0 to 100)
    double latency_ms;          ///< Total evaluation latency in milliseconds
    size_t token_count;         ///< Number of tokens evaluated
};

/**
 * @class NeuralEngine
 * @brief Thread-safe neural inference engine.
 */
class NeuralEngine {
public:
    NeuralEngine() : env_(ORT_LOGGING_LEVEL_WARNING, "LayaEngine") {}

    /**
     * @brief Initializes the neural model and tokenizer.
     * @param model_path Path to model_quantized.onnx.
     * @param tokenizer_path Path to tokenizer.json.
     * @param num_threads Number of intra-op threads per session.
     * @return True if initialized successfully.
     */
    bool init(const std::string& model_path, const std::string& tokenizer_path, int num_threads = 8) {
        if (!tokenizer_.load(tokenizer_path)) {
            std::cerr << "[NeuralEngine] Failed to load tokenizer from " << tokenizer_path << std::endl;
            return false;
        }

        try {
            session_options_.SetIntraOpNumThreads(num_threads);
            session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), session_options_);
            memory_info_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            std::cout << "[NeuralEngine] Initialized ONNX Session (" << model_path << ") successfully." << std::endl;
            return true;
        } catch (const Ort::Exception& e) {
            std::cerr << "[NeuralEngine] ONNX Init Exception: " << e.what() << std::endl;
            return false;
        }
    }

    /**
     * @brief Evaluates an incoming text prompt through the cascaded DLP + neural pipeline.
     * @param text Raw prompt text received from client.
     * @return InferenceVerdict with classification, score, and latency.
     * @note Thread-Safety: Ort::Session::Run is thread-safe for concurrent read-only invocations.
     */
    InferenceVerdict evaluate(const std::string& text) const {
        auto start = std::chrono::high_resolution_clock::now();
        InferenceVerdict result;
        result.decision = "choice";

        // Tier-1: Ultra-Fast Microsecond DLP / Secret Pre-Filter
        if (text.find("AKIA") != std::string::npos || text.find("sk-ant-") != std::string::npos || text.find("ghp_") != std::string::npos) {
            auto end = std::chrono::high_resolution_clock::now();
            result.verdict = "SECRET_LEAK";
            result.confidence = 1.0;
            result.score = 99;
            result.token_count = 0;
            result.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
            return result;
        }

        // Tier-2: Neural Tokenization in C++
        std::vector<int64_t> input_ids = tokenizer_.encode(text);
        size_t seq_len = input_ids.size();
        std::vector<int64_t> attention_mask(seq_len, 1);
        std::vector<int64_t> input_shape = {1, static_cast<int64_t>(seq_len)};

        try {
            Ort::Value in_ids = Ort::Value::CreateTensor<int64_t>(
                memory_info_, input_ids.data(), input_ids.size(), input_shape.data(), input_shape.size()
            );
            Ort::Value att_mask = Ort::Value::CreateTensor<int64_t>(
                memory_info_, attention_mask.data(), attention_mask.size(), input_shape.data(), input_shape.size()
            );

            const char* input_names[] = {"input_ids", "attention_mask"};
            const char* output_names[] = {"logits"};

            std::vector<Ort::Value> inputs;
            inputs.push_back(std::move(in_ids));
            inputs.push_back(std::move(att_mask));

            // Neural Forward Pass
            auto outputs = session_->Run(
                Ort::RunOptions{nullptr}, input_names, inputs.data(), inputs.size(), output_names, 1
            );

            // Parse Logits & Softmax: [SAFE (0), INJECTION (1)]
            float* logits = outputs[0].GetTensorMutableData<float>();
            float max_l = std::max(logits[0], logits[1]);
            float e_safe = std::exp(logits[0] - max_l);
            float e_inj = std::exp(logits[1] - max_l);
            float sum_e = e_safe + e_inj;
            float p_safe = e_safe / sum_e;
            float p_inj = e_inj / sum_e;

            auto end = std::chrono::high_resolution_clock::now();
            result.token_count = seq_len;
            result.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

            if (p_inj > 0.5f) {
                result.verdict = "PROMPT_INJECTION";
                result.confidence = p_inj;
                result.score = static_cast<int>(p_inj * 100.0f);
            } else {
                result.verdict = "SAFE";
                result.confidence = p_safe;
                result.score = static_cast<int>((1.0f - p_safe) * 100.0f);
            }
        } catch (const Ort::Exception& e) {
            std::cerr << "[NeuralEngine] Inference Error: " << e.what() << std::endl;
            result.verdict = "ERROR";
            result.confidence = 0.0;
            result.score = -1;
        }

        return result;
    }

private:
    UnigramTokenizer tokenizer_;
    Ort::Env env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memory_info_{nullptr};
};

} // namespace laya
