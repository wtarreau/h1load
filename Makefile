TOPDIR     := $(PWD)
DESTDIR    :=
PREFIX     := /usr/local
LIBDIR     := $(PREFIX)/lib

CROSS_COMPILE :=

# comment out this line or pass "USE_SSL=" with any value other than "1" to
# "make" to disable SSL. It may be needed to force SSL_CFLAGS to include
# certain paths, and SSL_LFLAGS to load libs from certain paths.
USE_SSL    := 1

CC         := $(CROSS_COMPILE)cc
OPT_CFLAGS := -O3
CPU_CFLAGS := -fomit-frame-pointer
DEB_CFLAGS := -Wall -g
DEF_CFLAGS :=
USR_CFLAGS :=
INC_CFLAGS :=
SSL_CFLAGS :=
THR_CFLAGS := -pthread
ifeq ($(USE_SSL),1)
USE_CFLAGS := -DUSE_SSL=1
else
USE_CFLAGS :=
endif
CFLAGS     := $(OPT_CFLAGS) $(CPU_CFLAGS) $(DEB_CFLAGS) $(DEF_CFLAGS) $(USR_CFLAGS) $(INC_CFLAGS) $(SSL_CFLAGS) $(THR_CFLAGS) $(USE_CFLAGS)

LD         := $(CC)
DEB_LFLAGS := -g
USR_LFLAGS :=
LIB_LFLAGS :=
ifeq ($(USE_SSL),1)
SSL_LFLAGS := -lssl -lcrypto
else
SSL_LFLAGS :=
endif
THR_LFLAGS := -pthread
LDFLAGS    := $(DEB_LFLAGS) $(USR_LFLAGS) $(LIB_LFLAGS) $(SSL_LFLAGS) $(THR_LFLAGS)

AR         := $(CROSS_COMPILE)ar
STRIP      := $(CROSS_COMPILE)strip
INSTALL    := install
BINS       := h1load
OBJS       :=
OBJS       += $(patsubst %.c,%.o,$(wildcard src/*.c))
OBJS       += $(patsubst %.S,%.o,$(wildcard src/*.S))

all: static shared tools

static: $(STATIC)

shared: $(SHARED)

tools: $(BINS)

h1load: h1load.o
	$(LD) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

install: install-tools

install-tools: tools
	$(STRIP) $(BINS)
	[ -d "$(DESTDIR)$(PREFIX)/bin/." ] || mkdir -p -m 0755 $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 0755 -t $(DESTDIR)$(PREFIX)/bin $(BINS)

clean:
	-rm -f $(BINS) $(OBJS) *.o *~
