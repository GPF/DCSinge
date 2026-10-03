# KallistiOS ##version##
#
# libmp3 Makefile

TARGET = libmp3.a
OBJS =
SUBDIRS = xingmp3 libmp3
LOCAL_CLEAN = build/*.o build/*.a
# SUBDIRS = mpglib libmp3

defaultall: create_kos_link $(OBJS) subdirs linklib

include $(KOS_BASE)/addons/Makefile.prefab

# creates the kos link to the headers
create_kos_link:
	rm -f $(KOS_PORTS)/include/mp3
	ln -s $(CURDIR)/include $(KOS_PORTS)/include/mp3

