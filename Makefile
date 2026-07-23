# Makefile for mft_reader
# Toolchain: TDM32 (g++ + windres), C++11
#
# Switch to TDM64 by overriding CXX/WINDRES on the command line, e.g.:
#   make CXX=d:/tdm64/bin/g++ WINDRES=d:/tdm64/bin/windres.exe

# CXX      := d:/tdm32/bin/g++.exe
CXX      := d:/llvm/bin/i686-w64-mingw32-clang++.exe
# CXX      := C:/cygwin64/bin/i686-w64-mingw32-g++.exe
WINDRES  := d:/llvm/bin/i686-w64-mingw32-windres.exe

# -D__USE_MINGW_ANSI_STDIO=1 fixes %llu (and other C99 format specifiers)
# on TDM32's mingw.org-based runtime: it reroutes printf/sprintf/etc.
# through GCC's own format implementation instead of the old msvcrt.dll
# one, which never learned the "ll" length modifier. Must be defined
# before any standard header is included, so we do it as a compiler
# flag rather than relying on source-file include order.
# CXXFLAGS := -std=g++11 -Wall -Wextra -O2 -D__USE_MINGW_ANSI_STDIO=1
CXXFLAGS := -std=c++11 -Wall -Wextra -O2 -static 
#CXXFLAGS += -DUNICODE -D_UNICODE

TARGET   := mft_reader.exe
SRC      := mft_reader.cpp
RC       := mft_reader.rc
MANIFEST := mft_reader.manifest
RES_OBJ  := mft_reader_res.o

.PHONY: all clean

all: $(TARGET)

# Final link pulls in both the compiled source and the compiled resource object 
# (the embedded manifest) so the elevation request ships inside the .exe.
$(TARGET): $(SRC) $(RES_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(RES_OBJ)

# windres compiles the .rc (which just points at the .manifest file) into
# a linkable COFF object.
$(RES_OBJ): $(RC) $(MANIFEST)
	$(WINDRES) $(RC) -O coff -o $(RES_OBJ)

clean:
	del /Q $(TARGET) $(RES_OBJ) 2>NUL || true
