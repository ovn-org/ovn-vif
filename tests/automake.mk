EXTRA_DIST += \
	$(TESTSUITE_AT) \
	$(SYSTEM_KMOD_TESTSUITE_AT) \
	$(TESTSUITE) \
	$(SYSTEM_KMOD_TESTSUITE) \
	tests/atlocal.in \
	$(srcdir)/package.m4 \
        $(srcdir)/tests/testsuite

TESTSUITE_AT = \
	tests/testsuite.at \
	tests/vif-plug-providers.at

SYSTEM_KMOD_TESTSUITE_AT = \
	tests/ovs-macros.at \
	tests/ovn-macros.at \
	tests/ofproto-macros.at \
	tests/system-common-macros.at \
	tests/system-kmod-testsuite.at \
	tests/system-kmod-macros.at \
	tests/system-plug-representor.at

TESTSUITE = $(srcdir)/tests/testsuite
TESTSUITE_DIR = $(abs_top_builddir)/tests/testsuite.dir
SYSTEM_KMOD_TESTSUITE = $(srcdir)/tests/system-kmod-testsuite
DISTCLEANFILES += tests/atconfig tests/atlocal tests/testsuite.log

AUTOTEST_PATH = $(ovs_builddir)/utilities:$(ovs_builddir)/vswitchd:$(ovs_builddir)/ovsdb:$(ovs_builddir)/vtep:tests:$(PTHREAD_WIN32_DIR_DLL):$(SSL_DIR):$(ovn_builddir)/controller-vtep:$(ovn_builddir)/northd:$(ovn_builddir)/utilities:$(ovn_builddir)/controller:$(ovn_builddir)/ic

export ovs_srcdir
export ovn_srcdir

# Run kmod tests. Assume kernel modules has been installed or linked into the
# kernel
check-kernel: all
	set $(SHELL) '$(SYSTEM_KMOD_TESTSUITE)' -C tests \
	AUTOTEST_PATH='$(AUTOTEST_PATH)'; \
	$(SUDO) "$$@" $(TESTSUITEFLAGS) -j1 || \
	(test X'$(RECHECK)' = Xyes && $(SUDO) "$$@" --recheck)

check-local:
	set $(SHELL) '$(TESTSUITE)' -C tests AUTOTEST_PATH=$(AUTOTEST_PATH); \
        "$$@" $(TESTSUITEFLAGS) || \
        (test -z "$$(find $(TESTSUITE_DIR) -name 'asan.*')" && \
         test X'$(RECHECK)' = Xyes && "$$@" --recheck)

AUTOTEST = $(AUTOM4TE) --language=autotest

$(SYSTEM_KMOD_TESTSUITE): package.m4 $(SYSTEM_TESTSUITE_AT) $(SYSTEM_KMOD_TESTSUITE_AT) $(COMMON_MACROS_AT)
	$(AM_V_GEN)$(AUTOTEST) -I '$(srcdir)' -o $@.tmp $@.at
	$(AM_V_at)mv $@.tmp $@

$(TESTSUITE): package.m4 $(TESTSUITE_AT) $(COMMON_MACROS_AT)
	$(AM_V_GEN)$(AUTOTEST) -I '$(srcdir)' -o $@.tmp $@.at
	$(AM_V_at)mv $@.tmp $@

# The `:;' works around a Bash 3.2 bug when the output is not writeable.
$(srcdir)/package.m4: $(top_srcdir)/configure.ac
	$(AM_V_GEN):;{ \
	  echo '# Signature of the current package.' && \
	  echo 'm4_define([AT_PACKAGE_NAME],      [$(PACKAGE_NAME)])' && \
	  echo 'm4_define([AT_PACKAGE_TARNAME],   [$(PACKAGE_TARNAME)])' && \
	  echo 'm4_define([AT_PACKAGE_VERSION],   [$(PACKAGE_VERSION)])' && \
	  echo 'm4_define([AT_PACKAGE_STRING],    [$(PACKAGE_STRING)])' && \
	  echo 'm4_define([AT_PACKAGE_BUGREPORT], [$(PACKAGE_BUGREPORT)])'; \
	} >'$(srcdir)/package.m4'

noinst_PROGRAMS += tests/ovstest
tests_ovstest_SOURCES = \
        tests/ovstest.h \
	tests/ovstest.c \
	lib/vif-plug-providers/representor/vif-plug-representor.c
tests_ovstest_LDADD = \
	$(OVS_LIBDIR)/libopenvswitch.la \
        lib/netlink-devlink.$(OBJEXT)

if HAVE_UDEV
tests_ovstest_LDADD += \
	-ludev
endif

tests_ovstest_CPPFLAGS = $(AM_CPPFLAGS)
tests_ovstest_CPPFLAGS += \
	-DOVSTEST \
	-I$(OVS_LIBDIR) \
	-Ilib/vif-plug-providers/representor

