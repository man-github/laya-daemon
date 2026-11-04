# ==============================================================================
# Laya AI Daemon - Carrier-Grade Production Makefile
# ==============================================================================

CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra -pthread
INCLUDES = -Iinclude -Iinclude/laya -I/usr/include/onnxruntime -I/usr/include/poppler/cpp

STATIC_ARCHIVE = /usr/lib/x86_64-linux-gnu/libarchive.a
ifeq ($(wildcard $(STATIC_ARCHIVE)),)
    ARCHIVE_LIB = -larchive
else
    ARCHIVE_LIB = $(STATIC_ARCHIVE) -lnettle -lacl -llzma -lzstd -llz4 -lbz2 -lz -lxml2
endif

LDFLAGS = -pthread -lonnxruntime -lpoppler-cpp -lre2 $(ARCHIVE_LIB)

BUILD_DIR = build
BINARIES = $(BUILD_DIR)/laya_daemon $(BUILD_DIR)/laya_client $(BUILD_DIR)/benchmark_client $(BUILD_DIR)/benchmark_per_core

.PHONY: all clean test deb install

all: $(BINARIES)
	@cp -f $(BUILD_DIR)/laya_daemon ./laya_daemon 2>/dev/null || true
	@cp -f $(BUILD_DIR)/laya_client ./laya_client 2>/dev/null || true
	@cp -f $(BUILD_DIR)/benchmark_client ./benchmark_client 2>/dev/null || true
	@cp -f $(BUILD_DIR)/benchmark_per_core ./benchmark_per_core 2>/dev/null || true
	@echo "=== All Laya AI binaries compiled successfully in $(BUILD_DIR)/ ==="

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/tinyxml2.o: src/tinyxml2.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/laya_daemon: src/daemon.cpp $(BUILD_DIR)/tinyxml2.o | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/laya_client: src/client.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BUILD_DIR)/benchmark_client: src/benchmark_client.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)

$(BUILD_DIR)/benchmark_per_core: src/benchmark_per_core.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)

test: all
	@echo "=== Running Laya Automated Regression Suite ==="
	@$(BUILD_DIR)/laya_client --test-suite

deb:
	@bash packaging/build_deb.sh

install: all
	install -d /usr/bin /etc/laya /var/log/laya /run/laya /usr/share/man/man1
	install -m 0755 $(BUILD_DIR)/laya_daemon /usr/bin/laya_daemon
	install -m 0755 $(BUILD_DIR)/laya_client /usr/bin/laya_client
	install -m 0755 $(BUILD_DIR)/benchmark_client /usr/bin/benchmark_client
	install -m 0755 $(BUILD_DIR)/benchmark_per_core /usr/bin/benchmark_per_core
	install -m 0644 config/laya.xml /etc/laya/laya.xml
	install -m 0644 config/laya_section.xml /etc/laya/laya_section.xml
	install -m 0644 config/patterns.json /etc/laya/patterns.json
	install -m 0644 config/test_suite.json /etc/laya/test_suite.json
	install -m 0644 systemd/laya-daemon.service /lib/systemd/system/laya-daemon.service
	@if [ -f man/man1/laya_client.1 ]; then install -m 0644 man/man1/laya_client.1 /usr/share/man/man1/laya_client.1; fi
	systemctl daemon-reload || true

clean:
	rm -rf $(BUILD_DIR) dist
	rm -f laya_daemon laya_client benchmark_client benchmark_per_core tinyxml2.o
