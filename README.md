# Laya AI Daemon: Multimodal & Multilingual System 1 Decision Engine

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![ONNX Runtime](https://img.shields.io/badge/Inference-ONNX%20Runtime%20v1.21-00599C.svg)](https://onnxruntime.ai/)
[![RE2 Engine](https://img.shields.io/badge/Regex-Google%20RE2%20Linear-4285F4.svg)](https://github.com/google/re2)
[![TinyXML2](https://img.shields.io/badge/XML%20Parser-Embedded%20TinyXML2-green.svg)](https://github.com/leethomason/tinyxml2)
[![IPC](https://img.shields.io/badge/IPC-Zero--Copy%20Abstract%20Sockets-brightgreen.svg)]()
[![License](https://img.shields.io/badge/License-Apache%202.0-orange.svg)](LICENSE)

**Laya Daemon** is an independent, carrier-grade, multimodal System 1 decision microservice engineered for high-throughput operational environments. Designed as a standalone profiling engine or inline proxy companion, it delivers ultra-low-latency text classification, dynamic Data Loss Prevention (DLP), multi-format document text extraction, and vision OCR in a single unified architecture.

---

## Architectural Highlights

```mermaid
flowchart TD
    subgraph Client ["Client Application / API Gateway"]
        App["Web Gateway / API / CLI"]
    end

    subgraph IPC ["Zero-Copy Unix Domain Socket (/tmp/laya.sock or @laya_server/K)"]
        Frame["Dual-Part Framing:\n[uint32_t meta_len][JSON Metadata (Epoch, Rules, MIME)]\n[uint32_t payload_len][Raw Binary / Document Bytes]"]
    end

    subgraph Daemon ["Laya Multimodal Daemon (laya_daemon)"]
        Config["Enterprise XML Engine (laya.xml)\nSIGHUP Hot-Reload via TinyXML2"]
        
        subgraph Pipeline ["Processing Pipeline"]
            DocEngine["In-Memory Document Extractor:\n- libarchive (docx, xlsx, pptx, odt, zip)\n- poppler-cpp (pdf)"]
            VisionEngine["Vision OCR Engine:\n- PaddleOCR PP-OCRv4 (Det + Rec)\n- Zero OpenMP Contention"]
            RE2Engine["DLP Engine (Google RE2):\n- Linear-Time Matching (No ReDoS)\n- LRU Cache with Idle TTL & Epoch Clearing"]
            DecisionEngine["System 1 Decision Engine:\n- Bidirectional ModernBERT / mmBERT\n- Single Forward Pass (~10-30ms)"]
        end

        Formatter["JSON Response Formatter\n(Templated from laya.xml)"]
    end

    App -->|Stream Request| Frame
    Frame --> DocEngine
    DocEngine -->|Extracted Text| RE2Engine
    DocEngine -->|Raw Images| VisionEngine
    VisionEngine -->|OCR Text| RE2Engine
    RE2Engine -->|Semantic Validation| DecisionEngine
    DecisionEngine --> Formatter
    Formatter -->|Structured JSON Response| App
```

---

## Core Capabilities

### 1. Multimodal Document & Vision Extraction
- **Zero Temporary Disk Files**: Decodes binary files directly from contiguous RAM buffers.
- **Office & Archive Streaming**: Uses static `libarchive.a` to unpack `.docx`, `.xlsx`, `.pptx`, `.odt`, `.ods`, `.zip`, and `.tar`, performing high-speed in-memory XML tag stripping.
- **Native PDF Decoding**: Extracts formatted textual contents using `poppler-cpp` (`poppler::document::load_from_raw_data()`).
- **Vision OCR**: Native ONNX Runtime PP-OCRv4 vision engine (`ch_PP-OCRv4_det.onnx` and `ch_PP-OCRv4_rec.onnx`) with single-threaded sequential execution, eliminating OpenMP CPU-pinning stalls.

### 2. High-Performance Stateless DLP with Google RE2
- **Dynamic Rule Ingestion**: Client applications or upstream proxies pass regex rules, weights, and pattern names on-the-fly inside the IPC metadata packet; the daemon remains completely stateless.
- **ReDoS Immunity**: Powered by Google RE2 to guarantee $O(N)$ linear-time execution with strict memory safety, immune to catastrophic backtracking attacks.
- **Intelligent LRU Cache**:
  - Thread-safe LRU cache storing precompiled RE2 objects.
  - Bounded capacity (`max_entries`) and idle TTL (`idle_ttl_seconds`) fully configurable in `laya.xml`.
  - **Epoch Invalidation**: Instantly flushes stale patterns when the client increments its `policy_epoch` or upon `SIGHUP`.

### 3. Multilingual System 1 Decision Model
- **Non-Autoregressive Architecture**: Ingests input state and classification schema simultaneously, outputting mathematically calibrated probabilities in a single forward pass (~10–30ms).
- **Zero Text Hallucination**: Does not generate unstructured free-form text; outputs discrete verdicts (`SAFE`, `PROMPT_INJECTION`, `SECRET_LEAK`, `MALICIOUS`).
- **100+ Languages**: Native support for multilingual routing via mmBERT and ModernBERT backbones.

### 4. Carrier-Grade Zero-Copy IPC
- **Linux Abstract Namespace Sockets**: Dedicated per-core channels (`@laya_server/<cpu>`) matching worker core affinity, bypassing filesystem locks and inode overhead.
- **Filesystem Unix Socket**: Shared `/tmp/laya.sock` socket for standard CLI tools.
- **Dual-Part Framing**: Binary payloads are transmitted raw without base64 CPU overhead or 33% memory expansion:
  ```
  +-------------------------+------------------------------------+
  | uint32_t meta_len (BE)  | JSON Metadata (UTF-8)              |
  +-------------------------+------------------------------------+
  | uint32_t payload_len(BE)| Raw Binary Document / Text Bytes   |
  +-------------------------+------------------------------------+
  ```

### 5. Enterprise Dual-XML Architecture
- **Runtime XML (`laya.xml`)**: Cleanly manages instance-specific daemon settings (`<daemon>`, `<models>`, `<regex_cache>`, `<response_template>`, typed `<item id="N">` rows).
- **Master Section Schema (`laya_section.xml`)**: Provides full UI descriptions, typed `<template>` schemas, and metadata definitions for WebUI consoles and appliance policy compilers.
- **Embedded TinyXML2**: `tinyxml2.h` and `tinyxml2.cpp` compiled directly into the binary; zero `.so` runtime dependencies.
- **Atomic `SIGHUP` Reload**: Reloads configuration and refreshes caches on `pkill -HUP laya_daemon` or `./laya_client --reload` without dropping socket connections.

### 6. Dynamic Model Auto-Discovery
- Automatically scans directories configured in `<models><directory path="..."/></models>`.
- Auto-deduces model roles based on conventional naming (`*det.onnx` for OCR detection, `*rec.onnx` for OCR recognition, `model*.onnx` for text decision).
- Pairs companion tokenizers (`tokenizer.json`) and dictionary keys (`ch_PP-OCR_keys_v1.txt`).
- Respects wildcard exclusions (`<exclude pattern="*_test.onnx,test_*"/>`).

---

## Installation & Deployment

### Method 1: Install via APT Repository (Debian / Ubuntu)

For production deployments on Debian (12 Bookworm, 13 Trixie) or Ubuntu (22.04 LTS, 24.04 LTS), install signed `.deb` packages directly via APT:

```bash
# 1. Install prerequisite transport and keyring packages
sudo apt-get update && sudo apt-get install -y ca-certificates curl gnupg

# 2. Import official Laya AI armored GPG public signing key into /etc/apt/keyrings/
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://repo.laya-ai.com/laya.gpg.key | sudo gpg --dearmor -o /etc/apt/keyrings/laya-archive-keyring.gpg
sudo chmod a+r /etc/apt/keyrings/laya-archive-keyring.gpg

# 3. Add the official Laya APT repository source list
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/laya-archive-keyring.gpg] https://repo.laya-ai.com/apt stable main" | sudo tee /etc/apt/sources.list.d/laya.list > /dev/null

# 4. Update package index and install laya-daemon
sudo apt-get update
sudo apt-get install -y laya-daemon

# 5. Verify daemon status and system service
sudo systemctl status laya-daemon
```

---

### Method 2: Build Debian Package (.deb) from Source

To produce a self-contained Debian release package locally:

```bash
# Generate the release debian package
make deb
# (or execute bash packaging/build_deb.sh directly)

# Install the generated package
sudo dpkg -i dist/laya-daemon_1.0.0_amd64.deb

# Verify systemd service startup
sudo systemctl status laya-daemon
```

---

### Method 3: Building from Source

#### Prerequisites (Debian 13 / Ubuntu 24.04)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
                        libonnxruntime-dev libpoppler-cpp-dev \
                        libre2-dev libarchive-dev libtinyxml2-dev
```

> [!NOTE]
> **Embedded vs System TinyXML2**:
> The repository vendors `tinyxml2.h` and `tinyxml2.cpp` directly under `include/laya/` and `src/` to compile statically into `tinyxml2.o` with zero shared library dependencies.

#### Compilation via Makefile

```bash
# Compile release binaries (laya_daemon, laya_client, benchmarks)
make -j$(nproc)

# Run automated regression test suite
make test

# Install binaries and configuration system-wide
sudo make install

# Clean build artifacts
make clean
```

#### Compilation via CMake

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j$(nproc)
```

---

## Repository Structure

```
laya/
├── .github/
│   └── workflows/
│       └── ci.yml             # GitHub Actions CI workflow (build & package test)
├── .gitignore                 # Standard build and runtime ignore rules
├── CMakeLists.txt             # Modern target-based CMake build configuration
├── Makefile                   # Production Makefile wrapper (all, test, deb, install)
├── LICENSE                    # Apache 2.0 open-source license
├── README.md                  # Project documentation & architecture manual
├── include/
│   └── laya/
│       ├── config.hpp         # Runtime XML configuration manager (TinyXML2)
│       ├── document.hpp       # In-memory document extraction (libarchive, poppler)
│       ├── engine.hpp         # ONNX Runtime System 1 neural decision engine
│       ├── formatter.hpp      # Dynamic templated JSON response formatter
│       ├── model_loader.hpp   # Model directory discovery & exclude scanner
│       ├── regex.hpp          # Google RE2 linear DLP scanner with LRU cache
│       ├── tokenizer.hpp      # Fast SentencePiece / Unigram C++ tokenizer
│       └── tinyxml2.h         # Embedded static XML parser header
├── src/
│   ├── daemon.cpp             # System service daemon entry point (laya_daemon)
│   ├── client.cpp             # CLI utility & regression test harness (laya_client)
│   ├── benchmark_client.cpp   # Multi-threaded IPC latency & throughput benchmark
│   ├── benchmark_per_core.cpp # Per-core lock-free abstract socket benchmark
│   └── tinyxml2.cpp           # Embedded XML parser implementation
├── config/
│   ├── laya.xml               # Runtime instance configuration
│   └── laya_section.xml       # Master section schema definition
├── systemd/
│   └── laya-daemon.service    # Production systemd unit file with sandboxing
├── debian/
│   ├── control                # Package metadata, architecture, dependencies
│   ├── conffiles              # Protected configuration files (/etc/laya/laya.xml)
│   ├── postinst               # Post-install script (service registration & restart)
│   ├── prerm                  # Pre-remove script (graceful service shutdown)
│   └── postrm                 # Post-remove script (purge directories & reload)
├── packaging/
│   └── build_deb.sh           # Automated Debian (.deb) release packager
├── scripts/
│   ├── setup_environment.sh   # Dependency setup script for Ubuntu/Debian
│   ├── download_models.sh     # ONNX model and tokenizer verification script
│   ├── run_simulation.py      # 20-scenario semantic validation benchmark
│   └── benchmark.py           # Multi-threaded Python latency benchmark
└── docs/                      # Blueprints, plans, reports, and testing guides
```

---

## System Service & Daemon Operation

### Systemd Service Management

When installed via APT or Debian package, `laya-daemon` is registered as a native systemd service:

```bash
# Check service status
sudo systemctl status laya-daemon

# Start / stop the service
sudo systemctl start laya-daemon
sudo systemctl stop laya-daemon

# Hot-reload XML configuration atomically without dropping connections (SIGHUP)
sudo systemctl reload laya-daemon

# Inspect active logs
journalctl -u laya-daemon -f
```

### Direct CLI Control & Options

```bash
./laya_daemon [options]
```

| Option | Argument | Description | Default Fallback |
| :--- | :--- | :--- | :--- |
| `-c`, `--config` | `<file_path>` | Explicit path to the XML configuration file. | Looks for `./config/laya.xml`, `./laya.xml`, then `/etc/laya/laya.xml`. |
| `-p`, `--pidfile` | `<file_path>` | Path to PID lock file. | `/run/laya/laya_daemon.pid` (or `/tmp/laya_daemon.pid`). |
| `-d`, `--daemon` | *(none)* | Detaches and runs in background as a daemon. | Foreground. |
| `--status` | *(none)* | Inspects PID file and reports whether daemon is running. | Returns exit code 0 if running, 3 if stopped. |
| `--reload` | *(none)* | Sends `SIGHUP` to running daemon to reload XML config. | — |
| `--stop` | *(none)* | Sends graceful `SIGTERM` to running daemon. | — |
| `-h`, `--help` | *(none)* | Displays command line help and options. | — |

### Common Launch Modes

```bash
# 1. Standard foreground execution (auto-detects ./laya.xml)
./laya_daemon

# 2. Explicit configuration path (development or staging)
./laya_daemon --config /etc/laya/laya.xml

# 3. Background production execution
nohup ./laya_daemon --config /etc/laya/laya.xml > /var/log/laya/laya_daemon.log 2>&1 &

# 4. Background execution in persistent tmux session
tmux new-session -d -s laya_daemon "cd /usr/local/src/laya-prototype && ./laya_daemon --config laya.xml"

# 5. Verify active socket endpoints
ss -xlp | grep laya
# Expected: LISTEN on /tmp/laya.sock and @laya_server/<0..N-1>
```

---

## Configuration Architecture: Dual-XML Model

Laya implements an enterprise dual-XML architectural pattern, cleanly separating the **runtime configuration instance** from the **master schema and WebUI specification**:

| XML File | Scope & Role | Consumed By | Key Responsibilities |
|---|---|---|---|
| [`laya.xml`](laya.xml) | **Runtime Instance Configuration** | `laya_daemon` | Live daemon values: active socket path, thread count, model directory list, active role bindings, exclusions, and response formatting. |
| [`laya_section.xml`](laya_section.xml) | **Master Section Schema & UI Definition** | WebUI Console & Policy Compilers | Schema metadata: data types (`BOOL`, `INT`, `STRING`, `STRING_SELECT_ONE`), field ordering (`n="0"`), WebUI tabs, table templates, dropdown choices, and CDATA operator documentation. |

---

### 1. Runtime Configuration: `laya.xml`

Placed at `/etc/laya/laya.xml` (or `./laya.xml` in local dev mode). This file is read at daemon launch and re-read atomically on `SIGHUP` without dropping client connections:

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

        <directory id="1">
            <enabled>TRUE</enabled>
            <comment>Primary ONNX models store</comment>
            <path>/usr/local/src/laya-prototype/model</path>
            <recursive>FALSE</recursive>
        </directory>

        <model id="10">
            <enabled>TRUE</enabled>
            <comment>Production ModernBERT Decision Model</comment>
            <filename>model_quantized.onnx</filename>
            <role>DECISION</role>
            <companion_path>/usr/local/src/laya-prototype/model/tokenizer.json</companion_path>
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
            <companion_path>/usr/local/src/laya-prototype/model/ch_PP-OCR_keys_v1.txt</companion_path>
        </model>

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

---

### 2. Master Section Schema: `laya_section.xml`

[`laya_section.xml`](laya_section.xml) defines the **formal schema contract** for enterprise policy compilers and WebUI consoles. It allows the Laya daemon to be managed and configured declaratively without requiring custom WebUI code.

#### Key Structural Sections in `laya_section.xml`:

1. **Section Header (`<laya>`)**:
   - `<pipeline_order>6</pipeline_order>`: Defines execution sequence in the policy filtering chain (e.g. alongside DLP).
   - `<parentgroup>security</parentgroup>` & `<subgroup>ai_safety</subgroup>`: Determines WebUI menu grouping.
   - `<logo>fa-brain</logo>`: FontAwesome icon displayed on the WebUI management dashboard.
   - `<desc><![CDATA[ ... ]]></desc>`: Rich HTML documentation displayed in the WebUI header.

2. **Global Parameters (`<global>`)**:
   Defines scalar knobs with ordinal positions (`n="0"`, `n="1"`), types (`BOOL`, `STRING`, `INT`), and inline help:
   - `<enabled n="0">`: Master toggle for the subsystem.
   - `<socket_path n="1">`: Filesystem IPC endpoint (`/tmp/laya.sock`).
   - `<worker_threads n="2">` & `<max_connections n="3">`: Concurrency limits.
   - `<cache_max_entries n="5">` & `<cache_idle_ttl n="6">`: RE2 LRU cache bounds.

3. **Row Templates (`<template>`)**:
   Defines the column schema for repeating table entries:
   - `<directory_entry>`: Columns for model auto-discovery folders (`path`, `recursive`, `enabled`, `comment`).
   - `<model_entry>`: Columns for explicit model role bindings (`filename`, `role`, `companion_path`, `enabled`, `comment`).
   - `<exclude_entry>`: Columns for blacklist wildcard rules (`pattern`, `enabled`, `comment`).

4. **Subsections / Tables (`<subsection>`)**:
   Instantiates multi-row management tables in the WebUI:
   - `<directories>`: Binds to template `directory_entry` for managing model search paths.
   - `<models>`: Binds to template `model_entry` for managing role overrides.
   - `<exclusions>`: Binds to template `exclude_entry` for managing blacklist filters.

5. **Enumeration Dictionaries (`<select>`)**:
   Defines validated choices for select fields (e.g. `<role>` offers `DECISION`, `OCR_DET`, and `OCR_REC`).

#### Schema Integration:
Enterprise WebUI consoles and configuration compilers parse `laya_section.xml` directly to generate interactive tabs, tables, tooltips, and validation schemas automatically.

---

## Operating `laya_client`

The `laya_client` CLI utility provides first-class debugging, scripting, and verification capabilities:

### 1. Evaluate Direct Prompts
```bash
./laya_client "Please review our AWS deployment settings"
# Output: {"verdict": "SAFE", "score": 0, "latency_ms": 156.58, "matched_patterns": []}

./laya_client "Here is the key: AKIAIOSFODNN7EXAMPLE"
# Output: {"verdict": "SECRET_LEAK", "score": 99, "latency_ms": 0.00, "matched_patterns": ["AWS_KEY"]}
```

### 2. Inspect Binary Documents (PDF & Office)
```bash
# Ingest PDF file in RAM
./laya_client --file /path/to/invoice.pdf

# Ingest Office Word (.docx) or Excel (.xlsx) file
./laya_client --file /path/to/contract.docx
```

### 3. Unix Pipe Streaming
```bash
# Stream document from curl or cat directly through laya_client
cat sensitive_doc.docx | ./laya_client -
```

### 4. Silent Exit Codes for Automated Bash Scripts
```bash
./laya_client --quiet --file sample.pdf && echo "TRAFFIC ALLOWED" || echo "TRAFFIC BLOCKED"
# Returns 0 on SAFE, 1 on THREAT / SECRET LEAK, 2 on Socket Error
```

### 5. Automated 10-Scenario Regression Test Suite
```bash
./laya_client --test-suite
```
Output:
```
=== Running Laya Automated Regression Test Suite ===

Test  1: Legitimate Query                       Expected: SAFE         Got: SAFE         [PASS]
Test  2: Weather Inquiry                        Expected: SAFE         Got: SAFE         [PASS]
Test  3: Code Development                       Expected: SAFE         Got: SAFE         [PASS]
Test  4: Prompt Injection - Jailbreak           Expected: PROMPT_INJ   Got: PROMPT_INJ   [PASS]
Test  5: Prompt Injection - System Override     Expected: PROMPT_INJ   Got: PROMPT_INJ   [PASS]
Test  6: Malicious Payload - Ransomware         Expected: MALICIOUS    Got: MALICIOUS    [PASS]
Test  7: Exploitation - CVE                     Expected: EXPLOIT      Got: EXPLOIT      [PASS]
Test  8: Secret Leak - AWS Key                  Expected: SECRET_LEAK  Got: SECRET_LEAK  [PASS]
Test  9: Secret Leak - GitHub Token             Expected: SECRET_LEAK  Got: SECRET_LEAK  [PASS]
Test 10: Harmless Math                          Expected: SAFE         Got: SAFE         [PASS]

Results: 10/10 passed.
```

### 6. Hot-Reloading Configuration
```bash
./laya_client --reload
# Output: Laya daemon successfully reloaded configuration: {"status":"RELOADED","service":"laya_daemon"}
```

---

## Future Multimodal & Multilingual Roadmap

1. **Multilingual mmBERT Backbone**: Upgrade from monolingual ModernBERT to multilingual mmBERT to provide native cross-lingual decision boundaries across 100+ languages without translation overhead.
2. **Audio Stream Profiling**: Integrate in-memory Whisper / Silero ONNX modules to inspect spoken audio payloads directly from multipart uploads.
3. **Multimodal Vision-Language Embeddings**: Integrate compact vision-language models (e.g. CLIP / MobileVLM) to semantically classify images, diagrams, and scanned charts beyond OCR text extraction.
4. **Distributed Clustering**: Support cluster-wide policy and model synchronizations across multi-node gateway topologies.

---

## License

Distributed under the Apache 2.0 License. See `LICENSE` for details.
