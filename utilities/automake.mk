if LINUX
noinst_PROGRAMS += utilities/devlink
utilities_devlink_SOURCES = utilities/devlink.c
utilities_devlink_LDADD = \
	lib/libovn-vif.la \
	-L$(OVS_LIBDIR) -lopenvswitch
endif
