# makefile for mft_reader
# SHELL=cmd.exe
USE_DEBUG = NO
USE_64BIT = YES
USE_UNICODE = YES
USE_CLANG = YES
# sadly, cygwin mingw does not support gdiplus...
USE_CYGWIN = NO

include ..\tool_select.mak 

MANIFEST := mft_reader.manifest

ifeq ($(USE_DEBUG),YES)
CFLAGS := -Wall -Wextra -g -c
LFLAGS := -g
else
CFLAGS := -Wall -Wextra -O3 -std=c++17 -c
LFLAGS := -s -O3 -mconsole
endif
CFLAGS += -Weffc++

ifeq ($(USE_UNICODE),YES)
CFLAGS += -DUNICODE -D_UNICODE
LFLAGS += -dUNICODE -d_UNICODE
endif

# This is required for *some* versions of makedepend
IFLAGS += -DNOMAKEDEPEND

ifeq ($(USE_STATIC),YES)
LFLAGS += -static
endif

BASE := mft_reader

BIN := $(BASE).exe

CPPSRC:=$(BASE).cpp 

OBJS := $(CPPSRC:.cpp=.o) rc.o

# LIBS:=-lshlwapi -lcomdlg32

#**************************************************************************
%.o: %.cpp
	$(TOOLS)/$(GNAME) $(CFLAGS) $< -o $@

#  build these targets even if there are files by these names
.PHONY: all clean

all: $(BIN)

clean:
	rm -f *.o *.exe *.zip

dist:
	rm -f $(BASE).zip
	zip $(BASE).zip $(BIN) README.md LICENSE.txt

wc:
	wc -l $(CPPSRC) $(BASE).rc

check:
	cmd /C "d:\llvm\bin\clang-tidy.exe $(CPPSRC)"

cppc:
	cmd /C "cppcheck --project=compile_commands.json --enable=all --check-level=exhaustive --suppressions-list=./.suppress.cppcheck"

clint:
	cmd /C "python ..\ClaudeLint.py --exclude der_libs"
	
depend: 
	makedepend $(IFLAGS) $(CPPSRC)

# note: though all other utilities can accept forward slash in paths,
#       windres cannot... 
rc.o: $(BASE).rc $(MANIFEST)
	$(TOOLS)/$(WRNAME) $< -O COFF -o $@

$(BIN): $(OBJS)
	$(TOOLS)/$(GNAME) $(OBJS) $(LFLAGS) -o $(BIN) $(LIBS) 

# DO NOT DELETE

mft_reader.o: mft_reader.h
