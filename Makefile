CC=gcc
CINCFLAGS=
ifdef TRACE	# Trace FUSE operations
	CTRACEFLAGS=-DTRACE
else
	CTRACEFLAGS=
endif
ifdef PROFILE
	CPROFFLAGS=-p -pg
else
	CPROFFLAGS=
endif
ifdef RELEASE
	COPTFLAGS=-O3 -s 
else
	COPTFLAGS=-O0 -ggdb3 -Werror -Wall
endif
CFLAGS=$(CINCFLAGS) $(COPTFLAGS) $(CPROFFLAGS) $(CTRACEFLAGS) `pkg-config fuse --cflags` 
LFLAGS=`pkg-config fuse --libs` 

PROJECT=scriptfs
SRC_DIR=src

all:$(PROJECT)

$(PROJECT):$(SRC_DIR)/scriptfs.c $(SRC_DIR)/procedures.o $(SRC_DIR)/operations.o
	@echo --------------- Linking of executable ---------------
	@$(CC) $(CFLAGS) -o $(PROJECT) $^ $(LFLAGS)

./%.o:%.c %.h
	@echo --------------- Compilation of $< ---------------
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@rm -f $(SRC_DIR)/*.o
	@rm -f $(PROJECT)

archive:
	@tar -cvjf $(PROJECT).tar.bz2 $(SRC_DIR)/*.c $(SRC_DIR)/*.h Makefile
