# Copyright (c) SUSE LLC
# SPDX-License-Identifier: GPL-2.0-or-later

TOPDIR := .
include $(TOPDIR)/Makefile.inc

# Check whether a function with name $1 has been declared in header file $2.
check_func = $(shell \
	if grep -Eq "^[^[:blank:]]+[[:blank:]]+$1[[:blank:]]*(.*)*" "$2"; then \
		found=1; \
		status="yes"; \
	else \
		found=0; \
		status="no"; \
	fi; \
	echo 1>&2 "Checking for $1 in $2 ... $$status"; \
	echo "$$found" \
	)

# Checker whether a file with name $1 exists
check_file = $(shell \
	if [ -f "$1" ]; then \
		found=1; \
		status="yes"; \
	else \
		found=0; \
		status="no"; \
	fi; \
	echo 1>&2 "Checking if $1 exists ... $$status"; \
	echo "$$found" \
	)

# Check whether a file contains a variable with name $1 in header file $2
check_var = $(shell \
	if grep -Eq "(^|[[:blank:]])$1([[:blank:]]|=|$$)" "$2"; then \
		found=1; \
		status="yes"; \
	else \
		found=0; \
		status="no"; \
	fi; \
	echo 1>&2 "Checking for $1 in $2 ... $$status"; \
	echo "$$found" \
	)

# Test special behavior of gcc 4.8 with nested initializers
# gcc 4.8 compiles blacklist.c only with -Wno-missing-field-initializers
TEST_MISSING_INITIALIZERS = $(shell \
	echo 'struct A {int a, b;}; struct B {struct A a; int b;} b = {.a.a=1};' | \
		$(CC) -c -Werror -Wmissing-field-initializers -o /dev/null -xc - >/dev/null 2>&1 \
	|| echo -Wno-missing-field-initializers)

DEFINES :=

ifneq ($(call check_func,dm_task_no_flush,$(devmapper_incdir)/libdevmapper.h),0)
	DEFINES += LIBDM_API_FLUSH
endif

ifneq ($(call check_func,dm_task_get_errno,$(devmapper_incdir)/libdevmapper.h),0)
	DEFINES += LIBDM_API_GET_ERRNO
endif

ifneq ($(call check_func,dm_task_set_cookie,$(devmapper_incdir)/libdevmapper.h),0)
	DEFINES += LIBDM_API_COOKIE
endif

ifneq ($(call check_func,udev_monitor_set_receive_buffer_size,$(libudev_incdir)/libudev.h),0)
	DEFINES += LIBUDEV_API_RECVBUF
endif

ifneq ($(call check_func,dm_task_deferred_remove,$(devmapper_incdir)/libdevmapper.h),0)
	DEFINES += LIBDM_API_DEFERRED
endif

ifneq ($(call check_func,dm_hold_control_dev,$(devmapper_incdir)/libdevmapper.h),0)
	DEFINES += LIBDM_API_HOLD_CONTROL
endif

ifneq ($(call check_var,ELS_DTAG_LNK_INTEGRITY,$(kernel_incdir)/scsi/fc/fc_els.h),0)
	DEFINES += FPIN_EVENT_HANDLER
	FPIN_SUPPORT = 1
endif

ifneq ($(call check_file,$(kernel_incdir)/linux/nvme_ioctl.h),0)
	ANA_SUPPORT := 1
endif

ifeq ($(ENABLE_DMEVENTS_POLL),0)
	DEFINES += -DNO_DMEVENTS_POLL
endif

SYSTEMD := $(strip $(or $(shell $(PKGCONFIG) --modversion libsystemd 2>/dev/null | awk '{print $$1}'), \
			$(shell systemctl --version 2>/dev/null | sed -n 's/systemd \([0-9]*\).*/\1/p')))


# $(call TEST_CC_OPTION,option,fallback)
# Test if the C compiler supports the option.
# Evaluates to "option" if yes, and "fallback" otherwise.
TEST_CC_OPTION = $(shell \
	if echo 'int main(void){return 0;}' | \
		$(CC) -o /dev/null -c -Werror "$(1)" -xc - >/dev/null 2>&1; \
	then \
		echo "$(1)"; \
	else \
		echo "$(2)"; \
	fi)

# "make" on some distros will fail on explicit '#' or '\#' in the program text below
__HASH__ := \#
# Check if _DFORTIFY_SOURCE=3 is supported.
# On some distros (e.g. Debian Buster) it will be falsely reported as supported
# but it doesn't seem to make a difference wrt the compilation result.
FORTIFY_OPT := $(shell \
	if /bin/echo -e '$(__HASH__)include <string.h>\nint main(void) { return 0; }' | \
		$(CC) -o /dev/null -c -O2 -Werror -D_FORTIFY_SOURCE=3 -xc - 2>/dev/null; \
	then \
		echo "-D_FORTIFY_SOURCE=3"; \
	else \
		echo "-D_FORTIFY_SOURCE=2"; \
	fi)

STACKPROT :=

all:	$(multipathdir)/autoconfig.h $(TOPDIR)/config.mk

$(multipathdir)/autoconfig.h:
	@echo creating $@
	@echo '#ifndef _AUTOCONFIG_H' >$@
	@echo '#define _AUTOCONFIG_H' >>$@
	@for x in $(DEFINES); do echo "#define $$x" >>$@; done
	@echo '#endif' >>$@

$(TOPDIR)/config.mk:
	@echo creating $@
	@echo "FPIN_SUPPORT := $(FPIN_SUPPORT)" >$@
	@echo "FORTIFY_OPT := $(FORTIFY_OPT)" >>$@
	@echo "SYSTEMD := $(SYSTEMD)" >>$@
	@echo "ANA_SUPPORT := $(ANA_SUPPORT)" >>$@
	@echo "STACKPROT := $(call TEST_CC_OPTION,-fstack-protector-strong,-fstack-protector)" >>$@
	@echo "ERROR_DISCARDED_QUALIFIERS := $(call TEST_CC_OPTION,-Werror=discarded-qualifiers,)" >>$@
	@echo "WNOCLOBBERED := $(call TEST_CC_OPTION,-Wno-clobbered -Wno-error=clobbered,)" >>$@
	@echo "WFORMATOVERFLOW := $(call TEST_CC_OPTION,-Wformat-overflow=2,)" >>$@
	@echo "W_MISSING_INITIALIZERS := $(call TEST_MISSING_INITIALIZERS)" >>$@
