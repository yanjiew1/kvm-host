include mk/common.mk

ARCH ?= $(shell uname -m)
PWD ?= $(shell pwd)

CC ?= gcc
CFLAGS = -O2
CFLAGS += -Wall -std=gnu99
CFLAGS += -g
CFLAGS += -Isrc
LDFLAGS = -lpthread

ifeq ($(ARCH), x86_64)
	CFLAGS += -DCONFIG_X86_64
	CFLAGS += -I$(PWD)/src/arch/x86_64
endif
ifeq ($(ARCH), aarch64)
	CFLAGS += -DCONFIG_AARCH64
	CFLAGS += -I$(PWD)/src/arch/aarch64
	CFLAGS += -DHAVE_LIBFDT
	ifneq ($(LIBFDT),)
		CFLAGS += -I$(LIBFDT)
		LDFLAGS += $(LIBFDT)/libfdt.a
	else
		LDFLAGS += -lfdt
	endif
endif

OUT ?= build
BIN = $(OUT)/kvm-host

all: $(BIN)

OBJS := \
	vm.o \
	serial.o \
	bus.o \
	pci.o \
	virtio-pci.o \
	virtq.o \
	virtio-blk.o \
	diskimg.o \
	main.o

ifeq ($(ARCH), x86_64)
	OBJS += arch/x86_64/vm.o \
			arch/x86_64/pci.o
endif
ifeq ($(ARCH), aarch64)
	OBJS += arch/aarch64/vm.o \
			arch/x86_64/pci.o \
			arch/aarch64/fdt.o
endif

OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: src/%.c
	$(Q)mkdir -p $(shell dirname $@)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

# Rules for downloading and building the minimal Linux system
include mk/external.mk

$(OUT)/ext4.img:
	$(Q)dd if=/dev/zero of=$@ bs=4k count=600
	$(Q)mkfs.ext4 -F $@

check: $(BIN) $(LINUX_IMG) $(ROOTFS_IMG) $(OUT)/ext4.img
	$(VECHO) "\nOnce the message 'Kernel panic' appears, press Ctrl-C to exit\n\n"
	$(Q)sudo $(BIN) -k $(LINUX_IMG) -i $(ROOTFS_IMG) -d $(OUT)/ext4.img

clean:
	$(VECHO) "Cleaning...\n"
	$(Q)rm -f $(OBJS) $(deps) $(BIN)

distclean: clean
	$(Q)rm -rf build

-include $(deps)
