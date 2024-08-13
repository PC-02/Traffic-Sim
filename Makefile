CC=mpicc

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = bin

# Flags
CFLAGS = -O2 -I$(INCDIR) 

# Source files
SRCS := $(wildcard $(SRCDIR)/*.c)

# Object files (none in this case)

# Executable name
EXECUTABLE = code

.PHONY: all clean

all: $(EXECUTABLE)

$(EXECUTABLE): $(SRCS)
	mkdir -p bin
	$(CC) $(CFLAGS) $^ -o bin/$@


init: 
	$(CC) $(CFLAGS) init_code.c -o init_code

clean:
	rm -rf bin init_code