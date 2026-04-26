FILENAME := 3ct
TARGET := $(shell uname -m)-$(shell uname -s | tr A-Z a-z)
EXE := $(FILENAME)_$(TARGET)

JOBS := $(shell nproc)
PUID := $(shell id -u)
PGID := $(shell id -g)

OUTPUT = build/$(EXE)

CC    := $(COMPILER_PREFIX)-gcc
CXX   := $(COMPILER_PREFIX)-g++
STRIP := $(COMPILER_PREFIX)-strip

ifeq ($(NDEBUG),1)
OPT := -O3 -flto -static
LDFLAGS += -Wl,--strip-all
else
OPT := -O0 -ggdb -ftrapv
endif

ifeq ($(SANITIZE),1)
OPT += -fsanitize=address,undefined
endif

CFLAGS = $(OPT) -Wall -Ivendored/
CXXFLAGS = $(OPT) -Wall -std=c++17 -Ivendored/
CPPFLAGS ?= -MMD -MP

SRCS_C   := $(wildcard src/*.c)
SRCS_CXX := $(wildcard src/*.cpp)

BUILDDIR = build/$(TARGET)
OBJS := $(SRCS_C:src/%.c=$(BUILDDIR)/%.c.o)
OBJS += $(SRCS_CXX:src/%.cpp=$(BUILDDIR)/%.cpp.o)
DEPS  = $(OBJS:.o=.d)


all: $(OUTPUT)

$(OUTPUT): builddir $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(OUTPUT) $(OBJS) $(LDFLAGS)

strip: $(OUTPUT)
	$(STRIP) --strip-all $(OUTPUT)

$(BUILDDIR)/%.c.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.cpp.o: src/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rfv build/

distclean: clean
	git clean -fdx

builddir:
	mkdir -p $(BUILDDIR)

release-base: clean
	$(MAKE) NDEBUG=1 -j$(JOBS) TARGET="$(shell uname -m)-$(shell uname -s | tr '[:upper:]' '[:lower:]')"
	$(MAKE) NDEBUG=1 -j$(JOBS) \
		CC="zig cc -target x86_64-linux-musl" \
		CXX="zig c++ -target x86_64-linux-musl" \
		STRIP="zig llvm-strip" \
		TARGET="x86_64-linux-musl"
	$(MAKE) NDEBUG=1 -j$(JOBS) \
		CC="zig cc -target aarch64-linux-musl" \
		CXX="zig c++ -target aarch64-linux-musl" \
		STRIP="zig llvm-strip" \
		TARGET="aarch64-linux-musl"
	$(MAKE) NDEBUG=1 -j$(JOBS) \
		CC="zig cc -target x86_64-windows-gnu" \
		CXX="zig c++ -target x86_64-windows-gnu" \
		STRIP="zig llvm-strip" \
		TARGET="x86_64-windows-gnu.exe"
	$(MAKE) NDEBUG=1 -j$(JOBS) \
		CC="zig cc -target aarch64-macos" \
		CXX="zig c++ -target aarch64-macos" \
		STRIP="zig llvm-strip" \
		TARGET="aarch64-macos" OPT="-O3"

release:
	podman build -t localhost/cxxbuilder buildtools/
	podman run --rm --userns=keep-id \
		-e HOME=/tmp \
		-e ZIG_GLOBAL_CACHE_DIR=/tmp/zig-global-cache \
		-e ZIG_LOCAL_CACHE_DIR=/tmp/zig-local-cache \
		-v ${PWD}:/src:Z localhost/cxxbuilder "/src/buildtools/podman-make-release"

.PHONY: clean distclean builddir release podman-release

-include $(DEPS)
