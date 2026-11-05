/**
 * @file laya_document.hpp
 * @brief High-Performance In-Memory Multi-Format Document Text Extractor.
 * @details Extracts text directly from memory buffers without temporary files on disk:
 *          - PDF documents: poppler-cpp (poppler::document::load_from_raw_data)
 *          - Office & Archives (.docx, .xlsx, .pptx, .odt, .ods, .tar, .zip): libarchive (archive_read_open_memory)
 *          - Format auto-detection via binary magic bytes.
 */

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <poppler-document.h>
#include <poppler-page.h>
#include <archive.h>
#include <archive_entry.h>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace laya {

/**
 * @enum DocumentType
 * @brief Detected binary document format.
 */
enum class DocumentType {
    UNKNOWN,
    PDF,
    ARCHIVE_OFFICE,
    IMAGE,
    PLAIN_TEXT
};

/**
 * @class DocumentExtractor
 * @brief Thread-safe in-memory text extraction for documents and archives.
 */
class DocumentExtractor {
public:
    /**
     * @brief Detects document type from leading magic bytes.
     */
    static DocumentType detect_type(const char* data, size_t len) {
        if (!data || len < 4) return DocumentType::UNKNOWN;

        // PDF magic: %PDF
        if (std::memcmp(data, "%PDF", 4) == 0) {
            return DocumentType::PDF;
        }

        // PKZIP magic: PK\x03\x04 (Office OpenXML, ODF, ZIP)
        if (std::memcmp(data, "PK\x03\x04", 4) == 0) {
            return DocumentType::ARCHIVE_OFFICE;
        }

        // JPEG magic: \xFF\xD8\xFF
        if (len >= 3 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8 && (unsigned char)data[2] == 0xFF) {
            return DocumentType::IMAGE;
        }

        // PNG magic: \x89PNG
        if (len >= 4 && (unsigned char)data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
            return DocumentType::IMAGE;
        }

        // TAR / GZ / 7Z / other archives supported by libarchive
        if (len >= 2 && (unsigned char)data[0] == 0x1F && (unsigned char)data[1] == 0x8B) { // GZIP
            return DocumentType::ARCHIVE_OFFICE;
        }
        if (len >= 6 && std::memcmp(data, "7z\xBC\xAF\x27\x1C", 6) == 0) { // 7Z
            return DocumentType::ARCHIVE_OFFICE;
        }

        return DocumentType::PLAIN_TEXT;
    }

    /**
     * @brief In-memory PDF text extraction using poppler-cpp.
     * @param data Pointer to raw PDF bytes.
     * @param len Size of buffer.
     * @return Extracted plain text.
     */
    static std::string extract_pdf(const char* data, size_t len) {
        std::string full_text;
        if (!data || len == 0) return full_text;

        std::unique_ptr<poppler::document> doc(
            poppler::document::load_from_raw_data(data, len)
        );

        if (!doc || doc->is_locked()) {
            return full_text;
        }

        int pages = doc->pages();
        for (int i = 0; i < pages; ++i) {
            std::unique_ptr<poppler::page> p(doc->create_page(i));
            if (p) {
                poppler::byte_array utf8 = p->text().to_utf8();
                full_text.append(utf8.data(), utf8.size());
                full_text += "\n";
            }
        }

        return full_text;
    }

    /**
     * @brief In-memory extraction from Office documents (.docx, .xlsx, .pptx, .odt) and archives via libarchive.
     * @param data Pointer to raw archive bytes.
     * @param len Size of buffer.
     * @return Extracted plain text with tags stripped.
     */
    static std::string extract_archive(const char* data, size_t len) {
        std::string full_text;
        if (!data || len == 0) return full_text;

        struct archive* a = archive_read_new();
        if (!a) return full_text;

        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);

        if (archive_read_open_memory(a, data, len) != ARCHIVE_OK) {
            archive_read_free(a);
            return full_text;
        }

        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            if (archive_entry_filetype(entry) != AE_IFREG) {
                archive_read_data_skip(a);
                continue;
            }

            const char* path = archive_entry_pathname(entry);
            if (!path) {
                archive_read_data_skip(a);
                continue;
            }

            std::string entry_name(path);

            // Check if this file is a text-bearing XML or text file inside the package
            bool is_target = false;
            if (entry_name.find("word/document.xml") != std::string::npos ||
                entry_name.find("xl/sharedStrings.xml") != std::string::npos ||
                entry_name.find("ppt/slides/slide") != std::string::npos ||
                entry_name.find("content.xml") != std::string::npos ||   // ODT / ODS
                entry_name.ends_with(".txt") ||
                entry_name.ends_with(".xml") ||
                entry_name.ends_with(".json") ||
                entry_name.ends_with(".csv")) {
                is_target = true;
            }

            if (!is_target) {
                archive_read_data_skip(a);
                continue;
            }

            // Read entry content into RAM buffer
            const void* buff = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            std::string raw_entry;

            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                raw_entry.append(static_cast<const char*>(buff), size);
            }

            // High-speed XML/HTML tag stripping
            if (entry_name.ends_with(".xml")) {
                bool in_tag = false;
                for (char c : raw_entry) {
                    if (c == '<') in_tag = true;
                    else if (c == '>') in_tag = false;
                    else if (!in_tag) full_text += c;
                }
                full_text += " ";
            } else {
                full_text.append(raw_entry);
                full_text += "\n";
            }
        }

        archive_read_free(a);
        return full_text;
    }

    /**
     * @brief In-memory OCR text extraction for image buffers (PNG, JPEG, WebP) via streaming pipes.
     * @param data Pointer to raw image bytes.
     * @param len Size of image buffer.
     * @return Extracted plain text via optical character recognition.
     */
    static std::string extract_image(const char* data, size_t len) {
        std::string full_text;
        if (!data || len == 0) return full_text;

        int in_pipe[2];
        int out_pipe[2];
        if (pipe(in_pipe) != 0) return full_text;
        if (pipe(out_pipe) != 0) {
            close(in_pipe[0]);
            close(in_pipe[1]);
            return full_text;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(in_pipe[0]);
            close(in_pipe[1]);
            close(out_pipe[0]);
            close(out_pipe[1]);
            return full_text;
        }

        if (pid == 0) {
            // Child process
            close(in_pipe[1]);
            close(out_pipe[0]);
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            close(in_pipe[0]);
            close(out_pipe[1]);

            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            execlp("tesseract", "tesseract", "stdin", "stdout", nullptr);
            _exit(127);
        }

        // Parent process
        close(in_pipe[0]);
        close(out_pipe[1]);

        // Stream image bytes to OCR engine in background thread to avoid pipe buffer deadlocks
        std::thread writer([in_fd = in_pipe[1], data, len]() {
            size_t written = 0;
            while (written < len) {
                ssize_t n = write(in_fd, data + written, len - written);
                if (n <= 0) break;
                written += n;
            }
            close(in_fd);
        });

        // Read extracted OCR text from output pipe
        char buf[4096];
        ssize_t n;
        while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0) {
            full_text.append(buf, n);
        }
        close(out_pipe[0]);

        if (writer.joinable()) {
            writer.join();
        }

        int status = 0;
        waitpid(pid, &status, 0);

        return full_text;
    }

    /**
     * @brief Automatically detects format and extracts plain text.
     */
    static std::string extract_auto(const char* data, size_t len) {
        DocumentType type = detect_type(data, len);
        if (type == DocumentType::PDF) {
            return extract_pdf(data, len);
        } else if (type == DocumentType::ARCHIVE_OFFICE) {
            return extract_archive(data, len);
        } else if (type == DocumentType::IMAGE) {
            return extract_image(data, len);
        }
        // Fallback: treat as plain UTF-8 text
        return std::string(data, len);
    }
};

} // namespace laya
