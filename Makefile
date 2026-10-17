# Build mpyhost against MicroPython v1.29.0.
# The interpreter sources are fetched on the first run.

ifeq ($(wildcard third_party/micropython/py/py.mk),)

all:
	git clone --depth 1 --branch v1.29.0 https://github.com/micropython/micropython.git third_party/micropython
	$(MAKE) all

else

MICROPYTHON_TOP := third_party/micropython
BUILD := build/gen
PACKAGE_DIR := build/micropython_embed
SHELL := C:/PROGRA~1/Git/usr/bin/sh.exe
export MICROPY_GIT_TAG := v1.29.0

include $(MICROPYTHON_TOP)/py/mkenv.mk

PYTHON := py -3.14
CC := C:/mingw64/bin/gcc.exe
CPP := $(CC) -E
# mingw32-make runs mkdir/cp/sed directly, so point them at Git's copies.
RM := C:/PROGRA~1/Git/usr/bin/rm.exe
CP := C:/PROGRA~1/Git/usr/bin/cp.exe
MKDIR := C:/PROGRA~1/Git/usr/bin/mkdir.exe
SED := C:/PROGRA~1/Git/usr/bin/sed.exe
CAT := C:/PROGRA~1/Git/usr/bin/cat.exe
TOUCH := C:/PROGRA~1/Git/usr/bin/touch.exe

include $(TOP)/py/py.mk

MICROPYTHON_EMBED_PORT := $(MICROPYTHON_TOP)/ports/embed
MICROPY_ROM_TEXT_COMPRESSION := 0

CFLAGS += -I. -I$(TOP) -I$(BUILD) -I$(MICROPYTHON_EMBED_PORT)
CFLAGS += -Wall -Werror -std=c99
CFLAGS += -D_WIN32_WINNT=0x0601

SRC_QSTR += src/roots.c

GENHDR_OUTPUT = $(addprefix $(BUILD)/genhdr/, \
	moduledefs.h \
	mpversion.h \
	qstrdefs.generated.h \
	root_pointers.h \
	)

.PHONY: all
all: dll

CLEAN_EXTRA := build/obj build/mpyhost.dll build/libmpyhost.a build/objs.txt $(PACKAGE_DIR)

.PHONY: clean-micropython-embed-package
clean-micropython-embed-package:
	rm -rf $(PACKAGE_DIR)

PACKAGE_DIR_LIST = $(addprefix $(PACKAGE_DIR)/,py extmod shared/runtime genhdr port)

.PHONY: micropython-embed-package
micropython-embed-package: $(GENHDR_OUTPUT)
	$(ECHO) "Generate micropython embed headers"
	$(Q)$(RM) -rf $(PACKAGE_DIR_LIST)
	$(Q)$(MKDIR) -p $(PACKAGE_DIR_LIST)
	$(Q)$(CP) $(TOP)/py/*.[ch] $(PACKAGE_DIR)/py
	$(Q)$(CP) $(TOP)/extmod/modplatform.h $(PACKAGE_DIR)/extmod
	$(Q)$(CP) $(TOP)/shared/runtime/gchelper.h $(PACKAGE_DIR)/shared/runtime
	$(Q)$(CP) $(TOP)/shared/runtime/gchelper_generic.c $(PACKAGE_DIR)/shared/runtime
	$(Q)$(CP) $(GENHDR_OUTPUT) $(PACKAGE_DIR)/genhdr
	$(Q)$(CP) $(MICROPYTHON_EMBED_PORT)/port/*.[ch] $(PACKAGE_DIR)/port

include $(TOP)/py/mkrules.mk

PY_SOURCES := $(wildcard $(MICROPYTHON_TOP)/py/*.c)
PY_OBJECTS := $(patsubst $(MICROPYTHON_TOP)/%.c,build/obj/%.o,$(PY_SOURCES))
HELPER_OBJECT := build/obj/shared/runtime/gchelper_generic.o
HOST_SOURCES := src/mpyhost.c src/node_mod.c src/node_gen.c src/vmport.c
HOST_OBJECTS := $(patsubst src/%.c,build/obj/host/%.o,$(HOST_SOURCES))

MPCFLAGS := -std=c99 -Wall -Werror -DNDEBUG -O2 -fvisibility=hidden -fno-strict-aliasing
MPCFLAGS += -D_WIN32_WINNT=0x0601
MPCFLAGS += -I. -Isrc -I$(BUILD) -I$(MICROPYTHON_TOP) -I$(MICROPYTHON_EMBED_PORT) -Isdk

dll: build/mpyhost.dll

build/mpyhost.dll: $(GENHDR_OUTPUT) $(PY_OBJECTS) $(HELPER_OBJECT) $(HOST_OBJECTS)
	$(MKDIR) -p build
	$(file >build/objs.txt,$(PY_OBJECTS) $(HELPER_OBJECT) $(HOST_OBJECTS))
	$(CC) -shared -o $@ @build/objs.txt -static-libgcc -Wl,--out-implib,build/libmpyhost.a

build/obj/%.o: $(MICROPYTHON_TOP)/%.c $(GENHDR_OUTPUT) mpconfigport.h src/roots.c
	$(MKDIR) -p $(dir $@)
	$(CC) $(MPCFLAGS) -c $< -o $@

build/obj/host/%.o: src/%.c $(GENHDR_OUTPUT) mpconfigport.h src/roots.c src/node_gen.c
	$(MKDIR) -p $(dir $@)
	$(CC) $(MPCFLAGS) -Wno-unused-parameter -c $< -o $@

src/node_gen.c: tools/gen_node.py sdk/node.h
	$(PYTHON) tools/gen_node.py

endif
