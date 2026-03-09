# ESP32-S3 Build System
#
# Compresses HTML and generates C header for embedding

.PHONY: build upload monitor clean menuconfig assets all

ENV ?= esp32s3_zero
PORT ?=

# Directories
DATA_DIR = data
BUILD_DIR = .build
INCLUDE_DIR = include

# HTML asset files
HTML_GZ = $(BUILD_DIR)/index.html.gz
HTML_HEADER = $(INCLUDE_DIR)/index_html.h

# Web UI source files
WEB_SOURCES = $(wildcard web/*.html web/*.css web/*.js web/components/*.html)

# Default target
all: build

# Asset generation
.PHONY: $(HTML_HEADER)
assets: $(HTML_HEADER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build web UI and compress in one step
$(HTML_GZ): $(WEB_SOURCES) | $(BUILD_DIR)
	@echo "Building and compressing web UI..."
	python3 web/build.py | gzip -9 > $@
	@echo "Compressed: $$(wc -c < $@) bytes"

$(HTML_HEADER): $(HTML_GZ)
	@echo "Generating header..."
	@echo "// Auto-generated - do not edit!" > $@
	@echo "// Source: $(HTML_SRC)" >> $@
	@echo "// Run 'make assets' to regenerate" >> $@
	@echo "" >> $@
	@echo "#pragma once" >> $@
	@echo "#include <stdint.h>" >> $@
	@echo "#include <stddef.h>" >> $@
	@echo "" >> $@
	@echo "static const size_t INDEX_HTML_GZ_LEN = $$(wc -c < $(HTML_GZ));" >> $@
	@echo "" >> $@
	@echo "static const uint8_t INDEX_HTML_GZ[] = {" >> $@
	xxd -i < $(HTML_GZ) | sed 's/^/    /' >> $@
	@echo "};" >> $@
	@echo "Generated $(HTML_HEADER)"

# PlatformIO targets
build: $(HTML_HEADER)
	pio run -e $(ENV)

upload: $(HTML_HEADER)
ifdef PORT
	pio run -e $(ENV) -t upload --upload-port $(PORT)
else
	pio run -e $(ENV) -t upload
endif

monitor:
ifdef PORT
	pio device monitor -p $(PORT)
else
	pio device monitor
endif

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(HTML_HEADER)
	rm -f sdkconfig.$(ENV)
	pio run -e $(ENV) -t fullclean

menuconfig:
	pio run -e $(ENV) -t menuconfig

# Force rebuild assets
rebuild-assets:
	rm -f $(HTML_HEADER) $(HTML_GZ)
	$(MAKE) assets
