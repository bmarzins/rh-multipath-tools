TOPDIR = ../..
include ../../Makefile.inc

CFLAGS += $(BIN_CFLAGS) -I$(mpathcmddir) -I..
LDFLAGS += $(BIN_LDFLAGS)
LIBDEPS += -L$(mpathcmddir) -lmpathcmd -L$(mpathvaliddir) -lmpathvalid \
	-lpthread -ldevmapper -ldl -ludev

EXEC = test

OBJS = test.o

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(EXEC) $(LDFLAGS) $(LIBDEPS)

clean: dep_clean
	$(RM) core *.o $(EXEC)

include $(wildcard $(OBJS:.o=.d))

dep_clean:
	$(RM) $(OBJS:.o=.d)
