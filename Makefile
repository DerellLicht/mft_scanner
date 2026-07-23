# Makefile for mft_reader
# Toolchain: TDM32 (g++ + windres), C++11
#
# Switch to TDM64 by overriding CXX/WINDRES on the command line, e.g.:
#   make CXX=d:/tdm64/bin/g++ WINDRES=d:/tdm64/bin/windres.exe

CXX      := d:/tdm32/bin/g++.exe
WINDRES  := d:/tdm32/bin/windres.exe
CXXFLAGS := -std=c++11 -Wall -Wextra -O2
TARGET   := mft_reader.exe
SRC      := mft_reader.cpp
RC       := mft_reader.rc
MANIFEST := mft_reader.manifest
RES_OBJ  := mft_reader_res.o

.PHONY: all clean

all: $(TARGET)

# Final link pulls in both the compiled source and the compiled resource
# object (the embedded manifest) so the elevation request ships inside
# the .exe.
$(TARGET): $(SRC) $(RES_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(RES_OBJ)

# windres compiles the .rc (which just points at the .manifest file) into
# a linkable COFF object.
$(RES_OBJ): $(RC) $(MANIFEST)
	$(WINDRES) $(RC) -O coff -o $(RES_OBJ)

clean:
	del /Q $(TARGET) $(RES_OBJ) 2>NUL || true
