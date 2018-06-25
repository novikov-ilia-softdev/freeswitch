include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_pipe

mod_LTLIBRARIES = mod_pipe.la
mod_pipe_la_SOURCES  = mod_pipe.c
mod_pipe_la_CFLAGS   = $(AM_CFLAGS)
mod_pipe_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_pipe_la_LDFLAGS  = -avoid-version -module -no-undefined -shared