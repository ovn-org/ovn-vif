lib_LTLIBRARIES += lib/libovn-vif.la
lib_libovn_vif_la_LDFLAGS = \
        $(OVS_LTINFO) \
        -Wl,--version-script=$(top_builddir)/lib/libovn-vif.sym \
        $(AM_LDFLAGS)
lib_libovn_vif_la_SOURCES = \
	lib/netlink-devlink.h \
	lib/netlink-devlink.c \
	lib/ovn-vif.c

DISTCLEANFILES += lib/.deps/netlink-devlink.Po

if ENABLE_PLUG_REPRESENTOR
lib_libovn_vif_la_SOURCES += \
	lib/vif-plug-providers/representor/vif-plug-representor.c
endif
