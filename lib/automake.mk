lib_LTLIBRARIES += lib/libovn-vif.la
lib_libovn_vif_la_LDFLAGS = \
        $(OVS_LTINFO) \
        -Wl,--version-script=$(top_builddir)/lib/libovn-vif.sym \
        $(AM_LDFLAGS)
lib_libovn_vif_la_SOURCES = \
	lib/netlink-devlink.h \
	lib/netlink-devlink.c
