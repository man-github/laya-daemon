# Implementation Plan: Enterprise Dual-XML Architecture, RE2 LRU Cache, libarchive & Zero-Copy Dual-Part IPC

## Executive Overview
Transform the **Laya Profiling Server** ([`E:\svn\laya_server\`](file:///E:/svn/laya_server/)) into a fully runtime-configurable, carrier-grade enterprise microservice.

Following our `/grill-me` architectural review, the system adopts:
1. **Enterprise Dual-XML Architecture**:
   - Runtime configuration: `laya.xml` managing instance configuration (`<section>`, global scalar tags, `<item id="N">` rows with `<enabled>TRUE</enabled>`, `<comment>`, and typed child elements).
   - Master section schema: `laya_section.xml` providing full WebUI tabs, table templates, dropdown choices, and CDATA descriptions.
   - Embedded static **TinyXML2** (`tinyxml2.h` / `tinyxml2.cpp`) for zero external `.so` dependencies.
2. **Stateless DLP Pattern Profiling with RE2 & Intelligent LRU Cache**:
   - Clients pass regex rules, pattern names, and a `policy_epoch` dynamically inside the IPC metadata payload; Laya remains completely stateless.
   - Powered by **Google RE2** for linear-time regex matching without catastrophic backtracking (ReDoS) vulnerability.
   - High-throughput **LRU compiled regex cache** with XML-configurable bounded capacity (`max_entries`) and idle TTL (`idle_ttl_seconds`).
   - Intelligent cache invalidation: instantly flushes on `SIGHUP` (daemon config reload) or when the client increments `policy_epoch`.
3. **Directory-Based ONNX Model Auto-Discovery**:
   - Auto-scans model directories at startup and reload.
   - Detects functional roles by standard convention (`*det.onnx` for OCR detection, `*rec.onnx` for OCR recognition, `model*.onnx` for text decision), with XML role override capabilities.
   - Respects `<exclude id="N"><pattern>...</pattern></exclude>` blacklist rules.
4. **In-Memory Multi-Format Document Extraction via `libarchive`**:
   - Replaces `libzip` with static `/usr/lib/x86_64-linux-gnu/libarchive.a`.
   - Streams `.docx`, `.xlsx`, `.pptx`, `.odt`, `.ods`, `.zip`, and `.tar` in RAM buffers via `archive_read_open_memory()`, stripping XML tags with zero disk I/O, alongside `poppler-cpp` for PDFs.
5. **Zero-Copy Dual-Part IPC Framing (HTTP Removed)**:
   - Strips HTTP/1.1 parsing from daemon core in favor of carrier-grade Unix socket framing:
     `[uint32_t meta_len][JSON metadata string][uint32_t payload_len][Raw binary document bytes]`
   - Direct zero-copy passing into memory parsers without base64 CPU overhead or 33% memory expansion.
6. **Configurable JSON Response Structures**:
   - `<response_template>` section in `laya.xml` allows administrators to customize output JSON field names, types, and toggled attributes without C++ recompilation.
7. **Carrier-Grade `laya_client` CLI Utility**:
   - CLI flags: direct prompt, `--file` binary document, stdin pipe `-`, `--quiet` exit codes (`0`=SAFE, `1`=THREAT, `2`=ERROR), `--benchmark`, `--reload`, and automated test suite.

---

## Architecture Flow

```mermaid
flowchart TD
    subgraph Client ["Client / Gateway Application"]
        SS_Core["Gateway Core Engine"]
        SS_Policies["DLP Patterns & Policy Epoch"]
    end

    subgraph IPC ["Unix Domain Socket (/tmp/laya.sock)"]
        Frame["Dual-Part Framing:\n[uint32_t meta_len][JSON Meta (Epoch, Rules, MIME)]\n[uint32_t payload_len][Raw Binary / Text Bytes]"]
    end

    subgraph LayaDaemon ["Laya AI Daemon (laya_daemon)"]
        Config["laya.xml (TinyXML2)\nSIGHUP Hot-Reload"]
        
        subgraph Pipeline ["Processing Pipeline"]
            DocExtractor["In-Memory Extraction:\nlibarchive (docx, xlsx, pptx, zip)\npoppler-cpp (pdf)"]
            VisionEngine["PP-OCRv4 Vision Engine:\nORT Sequential, IntraOp=1"]
            RE2Cache["RE2 Regex Engine + LRU Cache:\nAuto-cleared on SIGHUP & Epoch Delta"]
            ModernBERT["Laya System 1 Decision Model:\nONNX Runtime ModernBERT"]
        end

        Formatter["JSON Response Formatter\n(Templated from laya.xml)"]
    end

    SS_Core -->|Dynamic Rules + Payload| Frame
    SS_Policies -.->|Policy Epoch| Frame
    Frame --> DocExtractor
    DocExtractor -->|Text| RE2Cache
    DocExtractor -->|Images| VisionEngine
    VisionEngine -->|OCR Text| RE2Cache
    RE2Cache --> ModernBERT
    ModernBERT --> Formatter
    Formatter -->|Structured JSON Verdict| SS_Core
```

---

## 1. Enterprise Dual-XML Configuration

### 1.1 Runtime Configuration: `laya.xml`
Placed at `/etc/laya/laya.xml` (or `./laya.xml` in dev mode):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<laya>
    <!-- Daemon Core Settings -->
    <daemon>
        <enabled>TRUE</enabled>
        <socket_path>/tmp/laya.sock</socket_path>
        <worker_threads>4</worker_threads>
        <max_connections>1024</max_connections>
        <read_timeout_ms>5000</read_timeout_ms>
        <socket_permissions>0660</socket_permissions>
    </daemon>

    <!-- RE2 Compiled Regex Cache Settings -->
    <regex_cache>
        <enabled>TRUE</enabled>
        <max_entries>512</max_entries>
        <idle_ttl_seconds>900</idle_ttl_seconds>
    </regex_cache>

    <!-- ONNX Model Directories & Blacklists -->
    <models>
        <enabled>TRUE</enabled>
        <intra_op_threads>1</intra_op_threads>
        <inter_op_threads>1</inter_op_threads>

        <!-- Auto-discovery directories -->
        <directory id="1">
            <enabled>TRUE</enabled>
            <comment>Primary ONNX models store</comment>
            <path>/usr/local/src/laya-prototype/model</path>
            <recursive>FALSE</recursive>
        </directory>

        <!-- Role bindings / overrides (Option 1) -->
        <model id="10">
            <enabled>TRUE</enabled>
            <comment>Production ModernBERT Decision Model</comment>
            <filename>model_quantized.onnx</filename>
            <role>DECISION</role>
            <tokenizer_path>/usr/local/src/laya-prototype/model/tokenizer.json</tokenizer_path>
        </model>

        <model id="12">
            <enabled>TRUE</enabled>
            <comment>PaddleOCR Detection Model</comment>
            <filename>ch_PP-OCRv4_det.onnx</filename>
            <role>OCR_DET</role>
        </model>

        <model id="14">
            <enabled>TRUE</enabled>
            <comment>PaddleOCR Recognition Model</comment>
            <filename>ch_PP-OCRv4_rec.onnx</filename>
            <role>OCR_REC</role>
            <dictionary_path>/usr/local/src/laya-prototype/model/ch_PP-OCR_keys_v1.txt</dictionary_path>
        </model>

        <!-- Exclusions / Blacklists -->
        <exclude id="20">
            <enabled>TRUE</enabled>
            <comment>Skip testing and intermediate checkpoints</comment>
            <pattern>*_test.onnx,test_*,*checkpoint*</pattern>
        </exclude>
    </models>

    <!-- Configurable Response Template -->
    <response_template>
        <enabled>TRUE</enabled>
        <format>JSON</format>
        <include_verdict>TRUE</include_verdict>
        <include_score>TRUE</include_score>
        <include_latency>TRUE</include_latency>
        <include_matched_patterns>TRUE</include_matched_patterns>
        <include_tokens>FALSE</include_tokens>
        <verdict_key>verdict</verdict_key>
        <score_key>score</score_key>
        <latency_key>latency_ms</latency_key>
        <matched_patterns_key>matched_patterns</matched_patterns_key>
    </response_template>
</laya>
```

### 1.2 Master Section Definition: `laya_section.xml`
Compatible with enterprise WebUI management consoles and schema compilers:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<section>
    <laya>
        <enabled>true</enabled>
        <comment>Laya AI Profiling Engine</comment>
        <parentgroup>security</parentgroup>
        <subgroup>antivirus</subgroup>
        <pipeline_order>6</pipeline_order>
        <pipeline_label>Laya AI Profiling</pipeline_label>
        <logo>fa-brain</logo>
        <wizard>0</wizard>
        <type>section</type>
        <flags>1</flags>
        <desc><![CDATA[
            <p><b>Laya AI Profiling Engine</b> provides ultra-low-latency non-autoregressive decision classification, DLP regex scanning, and vision OCR.</p>
        ]]></desc>
        <global>
            <enabled n="0">
                <comment>Enabled</comment>
                <value>laya_enable</value>
                <type>BOOL</type>
                <desc><![CDATA[Master toggle for Laya AI daemon communication.]]></desc>
            </enabled>
            <socket_path n="1">
                <comment>Socket Path</comment>
                <value>/tmp/laya.sock</value>
                <type>STRING</type>
                <desc><![CDATA[Path to the Laya daemon Unix domain socket.]]></desc>
            </socket_path>
            <cache_max_entries n="2">
                <comment>Max Regex Cache Entries</comment>
                <value>512</value>
                <type>INT</type>
                <desc><![CDATA[Maximum number of compiled RE2 patterns kept in memory.]]></desc>
            </cache_max_entries>
            <cache_idle_ttl n="3">
                <comment>Cache Idle TTL (seconds)</comment>
                <value>900</value>
                <type>INT</type>
                <desc><![CDATA[Idle seconds before an unused compiled regex pattern is purged.]]></desc>
            </cache_idle_ttl>
        </global>
        <template>
            <directory_entry>
                <enabled n="0"><comment>Enabled</comment><value>enabled</value><type>BOOL</type></enabled>
                <comment n="1"><comment>Comment</comment><value>Description</value><type>STRING</type></comment>
                <path n="2"><comment>Model Directory Path</comment><value>/usr/local/src/laya-prototype/model</value><type>STRING</type></path>
                <recursive n="3"><comment>Recursive Scan</comment><value>FALSE</value><type>BOOL</type></recursive>
            </directory_entry>
            <model_entry>
                <enabled n="0"><comment>Enabled</comment><value>enabled</value><type>BOOL</type></enabled>
                <comment n="1"><comment>Comment</comment><value>Model Description</value><type>STRING</type></comment>
                <filename n="2"><comment>ONNX Filename</comment><value>model.onnx</value><type>STRING</type></filename>
                <role n="3"><comment>Role</comment><value>DECISION</value><type>STRING_SELECT_ONE</type></role>
            </model_entry>
            <exclude_entry>
                <enabled n="0"><comment>Enabled</comment><value>enabled</value><type>BOOL</type></enabled>
                <comment n="1"><comment>Comment</comment><value>Exclude reason</value><type>STRING</type></comment>
                <pattern n="2"><comment>Filename Wildcards</comment><value>*.tmp</value><type>STRING</type></pattern>
            </exclude_entry>
        </template>
        <subsection>
            <directories><enabled>true</enabled><comment>Model Directories</comment><template>directory_entry</template></directories>
            <models><enabled>true</enabled><comment>Model Role Bindings</comment><template>model_entry</template></models>
            <exclusions><enabled>true</enabled><comment>Model Exclusions</comment><template>exclude_entry</template></exclusions>
        </subsection>
    </laya>
</section>
```

---

## 2. RE2 Regular Expression Engine & Intelligent LRU Cache

### 2.1 Stateless Request-Specified DLP Patterns
Clients pass patterns inside the JSON metadata block:
```json
{
  "policy_epoch": 42,
  "mime": "application/pdf",
  "patterns": [
    {"name": "AWS_KEY", "regex": "AKIA[0-9A-Z]{16}", "verdict": "SECRET_LEAK", "score": 99},
    {"name": "GITHUB_PAT", "regex": "ghp_[0-9a-zA-Z]{36}", "verdict": "SECRET_LEAK", "score": 99}
  ]
}
```

### 2.2 Intelligent LRU Cache Design
- **Key**: 64-bit FNV-1a or `std::hash<std::string>` of the regex pattern string.
- **Value**: Shared pointer to `std::shared_ptr<re2::RE2>`, accompanied by `last_accessed_timestamp`.
- **Eviction Triggers**:
  1. **Policy Epoch Delta**: If `req.policy_epoch != current_cached_epoch`, the entire cache is invalidated immediately (client reloaded its DLP policy).
  2. **`SIGHUP` Reload**: Invalidate all compiled regexes on daemon config reload.
  3. **Capacity Overflow**: When `cache.size() >= max_entries`, evict the Least Recently Used (LRU) entry.
  4. **Idle TTL**: Any entry where `(now - last_accessed) > idle_ttl_seconds` is purged during periodic or on-access sweeps.
- **Linear Time Safety**: Google RE2 guarantees $O(N)$ execution time with strict memory limits, totally immune to ReDoS catastrophic backtracking.

---

## 3. In-Memory Multi-Format Document Extraction via `libarchive`

Replace `libzip` with static `/usr/lib/x86_64-linux-gnu/libarchive.a`:
- Stream buffers directly into `archive_read_open_memory()`.
- Unpack XML entries (`word/document.xml`, `xl/sharedStrings.xml`, `ppt/slides/slide*.xml`, `content.xml`).
- Perform high-speed in-place XML tag stripping (`<[^>]*>`) in RAM, preserving whitespace and alphanumeric content.
- Native PDF extraction via `poppler-cpp` (`poppler::document::load_from_raw_data()`).
- Zero disk temporary files, zero I/O bottlenecks.

---

## 4. Zero-Copy Dual-Part IPC Framing

### 4.1 Frame Structure over `/tmp/laya.sock`
```
+---------------------------+------------------------------------+
| uint32_t meta_len (BE)    | JSON Metadata String (UTF-8)       |
+---------------------------+------------------------------------+
| uint32_t payload_len (BE) | Raw Binary / Document Bytes        |
+---------------------------+------------------------------------+
```

### 4.2 Zero-Copy Advantages
- **Eliminates Base64**: Saves 33% payload memory and avoids encode/decode CPU cycles on 10MB+ PDF/Office files.
- **Direct Pointers**: The daemon receives `meta_len`, parses the small JSON header into a struct, then reads `payload_len` directly into a contiguous memory buffer passed straight to `poppler::document::load_from_raw_data()` or `archive_read_open_memory()`.

---

## 5. Enhanced `laya_client` CLI Tool

Provide rich developer and operator guidance:
```bash
# 1. Evaluate direct text prompt
./laya_client "Please review AWS_KEY AKIAIOSFODNN7EXAMPLE"

# 2. Inspect a local binary document or image
./laya_client --file /path/to/invoice.pdf

# 3. Stream data via Unix pipe
cat sensitive_contract.docx | ./laya_client -

# 4. Silent exit codes for bash scripting
./laya_client --quiet --file sample.pdf && echo "SAFE" || echo "THREAT DETECTED"

# 5. Hot-reload daemon configuration
./laya_client --reload

# 6. Run automated regression test suite & benchmarks
./laya_client --test-suite
./laya_client --benchmark 5000
```

---

## 6. Implementation Roadmap

| Phase | Task | Details |
|---|---|---|
| **Phase 1** | **Embedded Dependencies** | Copy `tinyxml2.h` / `tinyxml2.cpp` into repo; verify `libre2.a` and `libarchive.a` static linkage. |
| **Phase 2** | **XML Schemas & Config Loader** | Implement `laya_config.hpp` parsing `laya.xml` (enterprise format); write `laya.xml` and `laya_section.xml`. |
| **Phase 3** | **RE2 Engine & Intelligent Cache** | Implement `laya_regex.hpp` with RE2, LRU eviction, idle TTL, and epoch/SIGHUP invalidation. |
| **Phase 4** | **Model Auto-Discovery** | Implement `laya_model_loader.hpp` scanning directories, detecting roles, and applying exclusions. |
| **Phase 5** | **`libarchive` Document Extractor** | Refactor `laya_document.hpp` to use `libarchive` in-memory streaming for Office/Zip formats. |
| **Phase 6** | **Dual-Part IPC Daemon** | Update `laya_daemon.cpp` to strip HTTP and implement `[meta_len][JSON][payload_len][bytes]` framing. |
| **Phase 7** | **Enhanced `laya_client`** | Update `laya_client.cpp` with dual-part IPC, pipe streaming, `--file`, `--reload`, and `--quiet` exit codes. |
| **Phase 8** | **Compilation & Remote Validation** | Compile on `gen5`, run regression suite, benchmark throughput, and sync to Windows for Source Insight. |

---

## User Decision & Approval
Please review this plan. If you are satisfied with this architecture, click **Proceed** or let me know to begin implementation!
