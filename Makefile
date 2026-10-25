PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
CLANG ?= clang
TARGET := hush.elf
WRAPPER := $(PS5_PAYLOAD_SDK)/bin/prospero-clang

CFLAGS := -O2 -Wall -Werror -fno-stack-protector -fno-plt -femulated-tls \
          -fvisibility-nodllstorageclass=default
LIBS := -lc -lSceLibcInternal -lSceNet -lkernel_web
# The PS5 toolchain links with --build-id=uuid, which makes every build differ
# byte for byte. Dropping it keeps rebuilds reproducible.
REPRO := -Wl,--build-id=none

.PHONY: all clean sums

all: $(TARGET)

$(TARGET): src/main.c
	@if [ ! -f "$(WRAPPER)" ]; then \
		echo "$(WRAPPER) not found; set PS5_PAYLOAD_SDK" >&2; exit 1; \
	fi
	@if $(WRAPPER) --version >/dev/null 2>&1; then \
		echo "building with the SDK toolchain"; \
		$(WRAPPER) $(CFLAGS) -Isrc -o $@ $< $(LIBS) $(REPRO); \
	else \
		echo "building with $(CLANG) against the SDK sysroot"; \
		SCE_PROSPERO_SDK_DIR=$(PS5_PAYLOAD_SDK) \
		PATH=$(PS5_PAYLOAD_SDK)/bin:$$PATH \
		$(CLANG) --target=x86_64-sie-ps5 -isysroot $(PS5_PAYLOAD_SDK) \
			-isystem $(PS5_PAYLOAD_SDK)/target/include \
			-L $(PS5_PAYLOAD_SDK)/target/lib \
			$(CFLAGS) -o $@ $< $(LIBS) $(REPRO); \
	fi

sums: $(TARGET)
	shasum -a 256 $(TARGET) > SHA256SUMS
	cat SHA256SUMS

clean:
	rm -f $(TARGET)
