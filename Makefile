# Makefile for com_subjective_user_input - Cross-platform KVM Client
#
# Build targets:
#   make linux    - Build for Linux
#   make windows  - Build for Windows (cross-compile with MinGW)
#   make macos    - Build for macOS
#   make all      - Build for current platform
#   make clean    - Remove built files
#   make install-deps-linux - Install Linux dependencies
#
# Requirements:
#   Linux:   libwebsockets-dev, libssl-dev, libx11-dev, libxtst-dev, libxrandr-dev
#   Windows: MinGW-w64 with libwebsockets, OpenSSL
#   macOS:   libwebsockets (via Homebrew), OpenSSL

# Detect OS
UNAME_S := $(shell uname -s)

# Common flags
CFLAGS = -Wall -Wextra -O2 -g
DEFINES = -D_GNU_SOURCE

# Output paths
OUTDIR = build/$(PLATFORM)
EXEEXT =
TARGET = $(OUTDIR)/com_subjective_user_input$(EXEEXT)

# Source files
SRCS = src/input_unified.c

#==============================================================================
# Linux Configuration
#==============================================================================
ifeq ($(UNAME_S),Linux)
    PLATFORM = linux
    CC = gcc
    CFLAGS += $(shell pkg-config --cflags libwebsockets 2>/dev/null)
    LIBS = -lwebsockets -lssl -lcrypto -lX11 -lXtst -lXrandr -lXfixes -lpthread -lm
    # Fallback if pkg-config not available
    ifeq ($(CFLAGS),)
        CFLAGS += -I/usr/include
    endif
endif

#==============================================================================
# macOS Configuration
#==============================================================================
ifeq ($(UNAME_S),Darwin)
    PLATFORM = macos
    CC = clang
    # Homebrew paths
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
    CFLAGS += -I$(BREW_PREFIX)/include
    LDFLAGS = -L$(BREW_PREFIX)/lib
    LIBS = -lwebsockets -lssl -lcrypto -framework CoreFoundation -framework IOKit \
           -framework ApplicationServices -framework Carbon
endif

#==============================================================================
# Windows Configuration (cross-compile from Linux/WSL)
#==============================================================================
ifeq ($(PLATFORM),windows)
    CC = x86_64-w64-mingw32-gcc
    EXEEXT = .exe
    LIBS = -lwebsockets -lssl -lcrypto -lws2_32 -luser32
    DEFINES += -DWIN32
endif

#==============================================================================
# Build Targets
#==============================================================================

.PHONY: all clean linux windows macos install-deps-linux help

all: $(TARGET)

$(TARGET): $(SRCS) | $(OUTDIR)
	$(CC) $(CFLAGS) $(DEFINES) -o $@ $^ $(LDFLAGS) $(LIBS)
	@echo "Built $(TARGET) for $(PLATFORM)"

$(OUTDIR):
	mkdir -p $(OUTDIR)

linux:
	$(MAKE) PLATFORM=linux all

windows:
	$(MAKE) PLATFORM=windows all

macos:
	$(MAKE) PLATFORM=macos all

clean:
	rm -f build/linux/com_subjective_user_input build/windows/com_subjective_user_input.exe build/macos/com_subjective_user_input com_subjective_user_input.exe com_subjective_user_input.o
	@echo "Cleaned build files"

#==============================================================================
# Dependency Installation
#==============================================================================

install-deps-linux:
	@echo "Installing Linux dependencies..."
	sudo apt-get update
	sudo apt-get install -y \
		build-essential \
		libwebsockets-dev \
		libssl-dev \
		libx11-dev \
		libxtst-dev \
		libxrandr-dev \
		libxfixes-dev \
		pkg-config
	@echo "Dependencies installed. You may need to add user to 'input' group:"
	@echo "  sudo usermod -a -G input $$USER"
	@echo "Then log out and back in."

install-deps-macos:
	@echo "Installing macOS dependencies..."
	brew install libwebsockets openssl
	@echo "Dependencies installed."

install-deps-windows:
	@echo "For Windows, install MSYS2 and run:"
	@echo "  pacman -S mingw-w64-x86_64-libwebsockets mingw-w64-x86_64-openssl"

#==============================================================================
# Help
#==============================================================================

help:
	@echo "Makefile for com_subjective_user_input - Cross-platform KVM Client"
	@echo ""
	@echo "Usage:"
	@echo "  make              - Build for current platform"
	@echo "  make linux        - Build for Linux"
	@echo "  make windows      - Build for Windows (cross-compile)"
	@echo "  make macos        - Build for macOS"
	@echo "  make clean        - Remove built files"
	@echo ""
	@echo "Dependencies:"
	@echo "  make install-deps-linux  - Install Linux dependencies (apt)"
	@echo "  make install-deps-macos  - Install macOS dependencies (brew)"
	@echo ""
	@echo "Running:"
	@echo "  ./com_subjective_user_input --help"
	@echo "  ./com_subjective_user_input --role main --server ws://192.168.1.100:8765"
	@echo "  ./com_subjective_user_input --role player --server ws://192.168.1.100:8765"

