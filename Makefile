# makefile for franklin app
# SHELL=cmd.exe
USE_DEBUG = NO
USE_64BIT = NO
USE_UNICODE = NO
USE_CLANG = YES
# use -static for clang/llvm and cygwin/mingw
USE_STATIC = YES

#  clang++ vs tdm g++
#  clang gives *much* clearer compiler error messages...
#  However, programs built with clang++ will require libc++.dll and libunwind.dll
#  in order to be used elsewhere unless built with -static, 
#  which significantly boosts file size)
# llvm:  374784 bytes
# tdm32: 232960 bytes
ifeq ($(USE_64BIT),YES)
TOOLS:=d:\tdm64\bin
GNAME:=g++
WRNAME:=windres.exe
else
ifeq ($(USE_CLANG),YES)
TOOLS:=d:\llvm\bin
GNAME:=i686-w64-mingw32-clang++.exe
WRNAME:=i686-w64-mingw32-windres.exe
else
TOOLS:=d:\tdm32\bin
GNAME:=g++
WRNAME:=windres.exe
endif
endif

MANIFEST := mft_reader.manifest

ifeq ($(USE_DEBUG),YES)
CFLAGS := -Wall -Wextra -g -c
LFLAGS := -g
else
CFLAGS := -Wall -Wextra -O3 -c
LFLAGS := -s -O3
endif
# CFLAGS += -Weffc++
ifeq ($(USE_64BIT),YES)
CFLAGS += -DUSE_64BIT
endif


ifeq ($(USE_UNICODE),YES)
CFLAGS += -DUNICODE -D_UNICODE
LiFLAGS += -dUNICODE -d_UNICODE
LFLAGS += -dUNICODE -d_UNICODE
endif

# This is required for *some* versions of makedepend
IFLAGS += -DNOMAKEDEPEND

ifeq ($(USE_STATIC),YES)
LFLAGS += -static
endif

ifeq ($(USE_64BIT),NO)
BASE := mft_reader
else
BASE := mft_reader64
endif
BIN := $(BASE).exe

CPPSRC:=$(BASE).cpp 

OBJS = $(CPPSRC:.cpp=.o) rc.o

LIBS:=-lshlwapi -lcomdlg32

#**************************************************************************
%.o: %.cpp
	$(TOOLS)/$(GNAME) $(CFLAGS) $< -o $@

all: $(BIN)

clean:
	rm -f $(OBJS) $(BIN) *.zip

dist:
	rm -f $(BASE).zip
	zip $(BASE).zip $(BIN) Readme.md LICENSE.txt

wc:
	wc -l $(CPPSRC)

cppc:
	cmd /C "cppcheck --project=compile_commands.json --check-level=exhaustive --enable=all --std=c++14 --suppressions-list=./.suppress.cppcheck"

check:
	cmd /C "d:\llvm\bin\clang-tidy.exe $(CPPSRC)"

depend: 
	makedepend $(IFLAGS) $(CPPSRC)

# note: though all other utilities can accept forward slash in paths,
#       windres cannot... 
rc.o: $(BASE).rc $(MANIFEST)
	$(TOOLS)\$(WRNAME) $< -O COFF -o $@

$(BIN): $(OBJS)
	$(TOOLS)/$(GNAME) $(OBJS) $(LFLAGS) -o $(BIN) $(LIBS) 

# DO NOT DELETE
