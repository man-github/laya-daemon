# `laya_client(1)` — Laya AI Multimodal Client Reference Manual

`laya_client` is the native C++ CLI utility and automated test runner for the **Laya AI Multimodal & Multilingual Decision Gateway**. It communicates directly with `laya_daemon` over high-performance local UNIX domain sockets (supporting both filesystem sockets and Linux abstract namespace domain sockets).

---

## 1. Overview & Architectural Role

`laya_client` provides a zero-dependency, carrier-grade interface for integrating, inspecting, and automating Laya AI security evaluations. It eliminates the need for raw socket wrappers (like `socat` or `nc`), complex byte-packing scripts, or external JSON parsers like `jq`.

### Key Capabilities

* **Zero Hardcoding**: All DLP rules, metadata, targets, and test cases can be passed via command-line flags or external configuration files.
* **Dual-Engine Support**: Triggers instant heuristic/DLP regex checks (Google RE2) and deep semantic intent classifications (ModernBERT / ONNX).
* **Multi-Format Ingestion**: Evaluates plain text prompts, standard input streams (`-`), and rich document formats (`.pdf`, `.docx`, `.xlsx`, `.txt`, `.csv`, images).
* **POSIX-Compliant Automation**: Returns standard exit codes (`0` for Safe, `1` for Threat, `2` for Error) and supports quiet single-token extraction (`-q`) for direct usage in Bash `if` statements.
* **Zero-`jq` Field Extraction**: Directly extracts JSON response keys (`--field score`, `--field latency_ms`, `--field matched_patterns`) without third-party tooling.
* **Abstract & Filesystem Socket Support**: Can address specific CPU worker cores directly (`@laya_server/<core_id>`) or filesystem socket endpoints (`/run/laya/laya.sock`, `/tmp/laya.sock`).
* **Automated Regression Harness**: Runs end-to-end regression suites (`--test-suite`) against JSON test books.
* **Live Daemon Control**: Issues hot-reload signals (`--reload`) across the socket to clear regex caches and reload `laya.xml`.

---

## 2. Command Synopsis

```bash
laya_client [OPTIONS] [TEXT]
laya_client [OPTIONS] -f <FILE_PATH>
cat <FILE_OR_STREAM> | laya_client [OPTIONS] -
laya_client --test-suite [SUITE_JSON]
laya_client --reload
```

---

## 3. Options Reference

### Input Options

| Option | Argument | Description |
| :--- | :--- | :--- |
| `<text>` | `string` | Direct plain text prompt passed as a command-line argument. |
| `-f`, `--file` | `<path>` | Path to a file on disk. Auto-detects format (PDF, Word DOCX, Excel XLSX, plain text, or images) and extracts text in-memory before evaluation. |
| `-` | *None* | Read payload bytes from standard input (`stdin`) until EOF (pipe mode). |

### Connection Options

| Option | Argument | Description |
| :--- | :--- | :--- |
| `-s`, `--socket` | `<path\|@name>` | Explicit UNIX domain socket path or abstract socket name.<br>• Filesystem socket: `/run/laya/laya.sock` or `/tmp/laya.sock`<br>• Abstract core socket: `@laya_server/0`, `@laya_server/1`, etc.<br>*(Default fallback: auto-detects `@laya_server/0`, then `/run/laya/laya.sock`, then `/tmp/laya.sock`)* |

### Metadata & DLP Rule Options

| Option | Argument | Description |
| :--- | :--- | :--- |
| `-p`, `--pattern` | `<spec>` | Pass an ad-hoc regex pattern to merge into the request evaluation. Can be repeated multiple times.<br>**Syntax**: `NAME=REGEX[:VERDICT[:SCORE]]`<br>• *Shorthand*: `-p "AWS_KEY=AKIA[0-9A-Z]{16}"` (defaults to `SECRET_LEAK`, score `90`)<br>• *Explicit*: `-p "CVE=CVE-[0-9]+:EXPLOIT:95"` |
| `--patterns-file` | `<path.json>` | Load an external DLP regex rulebook in JSON format (overrides or supplements default `/etc/laya/patterns.json`). |
| `-m`, `--meta` | `<json>` | Pass an explicit, arbitrary JSON metadata string to accompany the payload framing. |
| `-M`, `--meta-file` | `<path.json>` | Read explicit JSON metadata directly from a file. |

### Output Formatting (Shell-Friendly)

| Option | Argument | Description |
| :--- | :--- | :--- |
| `-q`, `--quiet` | *None* | Suppress full JSON output. Prints **ONLY** the final verdict token (`SAFE`, `SECRET_LEAK`, `PROMPT_INJECTION`, `MALICIOUS`, `EXPLOIT`, `ERROR`). |
| `--field` | `<key>` | Extracts and prints the raw scalar value of a specific JSON field from the response (e.g. `score`, `latency_ms`, `verdict`, `matched_patterns`). |

### Daemon Control & Test Harness

| Option | Argument | Description |
| :--- | :--- | :--- |
| `--test-suite` | `[file.json]` | Executes an automated regression test suite against a JSON test catalog. Defaults to `/etc/laya/test_suite.json`. |
| `--reload` | *None* | Transmits a framed `{"cmd":"RELOAD"}` control message to `laya_daemon`, instructing it to re-parse `/etc/laya/laya.xml`, clear compiled RE2 cache, and refresh worker configurations. |
| `-h`, `--help` | *None* | Displays command syntax, flag descriptions, and exit code reference. |

---

## 4. Exit Codes (POSIX Standard)

`laya_client` sets standard process exit codes so it can be evaluated directly in shell conditions without text parsing:

| Exit Code | Classification | Meaning | Recommended Action |
| :---: | :--- | :--- | :--- |
| **`0`** | **SAFE / ALLOW** | The payload was evaluated as safe (severity score `< 50` and no DLP patterns matched). | Allow traffic / proceed. |
| **`1`** | **THREAT / DENIED** | High-severity risk detected (Prompt Injection, Secret Leak, Malware, Exploit, or score `≥ 50`). | Block traffic / alert security. |
| **`2`** | **ERROR** | Connection failure, socket unreachable, file not found, or malformed protocol framing. | Log system error / fail-safe open or closed based on policy. |

---

## 5. Shell Scripting Recipes & Examples

### Recipe 1: Simple Shell Guard (Clean Exit-Code Branching)

```bash
if laya_client -q "Please summarize our company VPN guidelines"; then
    echo "Traffic permitted."
else
    echo "Threat detected! HTTP 403 Forbidden."
fi
```

### Recipe 2: Extracting Verdict and Latency for Logging

```bash
PROMPT="My AWS key is AKIAIOSFODNN7EXAMPLE please test it"

VERDICT=$(laya_client -q "$PROMPT")
LATENCY=$(laya_client --field latency_ms "$PROMPT")
SCORE=$(laya_client --field score "$PROMPT")

echo "Audit: verdict=$VERDICT score=$SCORE latency=${LATENCY}ms"
# Output: Audit: verdict=SECRET_LEAK score=99 latency=0.00ms
```

### Recipe 3: Scanning Documents from Disk or Stdin

```bash
# Direct document scanning (PDF text extraction)
laya_client -q --file /var/local/test/contract.pdf

# Piping downloaded or streamed files directly
curl -s "https://internal.corp/spec.docx" | laya_client -q -
```

### Recipe 4: Ad-Hoc Dynamic Rule Testing

```bash
# Enforce a custom project codename check on the fly
laya_client -p "PROJECT_LEAK=PROJECT_TITAN_[0-9]{4}:SECRET_LEAK:99" \
            "Status update on PROJECT_TITAN_4412 deployment"
```

### Recipe 5: Verifying Specific Core Worker Threads

```bash
# Target CPU worker core 2 directly via abstract domain socket
laya_client -s "@laya_server/2" "Harmless test query"
```

### Recipe 6: Hot-Reloading the Daemon from CI/CD

```bash
# Push new patterns.json or laya.xml and reload instantly
laya_client --reload
```

---

## 6. IPC Binary Wire Protocol Specification

When `laya_client` speaks to `laya_daemon`, it uses a low-overhead, 2-part framed binary protocol over `AF_UNIX`:

```text
+-----------------------+-----------------------+-----------------------+-----------------------+
|  4 Bytes (Big-Endian) |      N Bytes UTF-8    |  4 Bytes (Big-Endian) |      M Bytes Binary   |
|     metadata_len (N)  |     metadata_json     |      payload_len (M)  |     raw_payload_data  |
+-----------------------+-----------------------+-----------------------+-----------------------+
```

1. **Metadata Header**: A 32-bit big-endian unsigned integer indicating the byte length of the JSON metadata object.
2. **Metadata Payload**: UTF-8 encoded JSON containing `policy_epoch`, dynamic `patterns`, or management commands (`{"cmd":"RELOAD"}`).
3. **Payload Header**: A 32-bit big-endian unsigned integer indicating the byte length of the body.
4. **Body Payload**: The raw prompt text or raw binary document (PDF, DOCX, XLSX, image).
5. **Daemon Response**: The daemon replies with a 4-byte big-endian response length followed by the UTF-8 JSON verdict object:
   ```json
   {"verdict": "SAFE", "score": 0, "latency_ms": 161.93, "matched_patterns": []}
   ```

---

## 7. Files & Default Paths

* **Binary**: `/usr/bin/laya_client`
* **Configuration**: `/etc/laya/laya.xml`
* **Baseline Patterns**: `/etc/laya/patterns.json`
* **Gitleaks Pattern Library**: `/etc/laya/patterns_gitleaks.json`
* **Regression Test Catalog**: `/etc/laya/test_suite.json`
* **Filesystem Socket**: `/tmp/laya.sock` (or `/run/laya/laya.sock`)
* **Abstract Sockets**: `@laya_server/0`, `@laya_server/1`, `@laya_server/2`, `@laya_server/3`

---

## 8. See Also

* `laya_daemon(8)` — Laya AI Multimodal Gateway Server
* `laya.xml(5)` — Laya Configuration and Template Specification
* `gitleaks(1)` — Secret detection rule syntax
