#lib_LTLIBRARIES = libimap.la
noinst_LIBRARIES = libimap.a

#libimap_la_SOURCES =  
libimap_a_SOURCES = \
	auth-cram.c	\
	imap.h		\
	imap-auth.c	\
	imap-auth.h	\
	imap-handle.c	\
	imap-handle.h	\
	imap-commands.c	\
	imap-commands.h	\
	imap-fetch.c	\
	imap-fetch.h	\
	libimap-marshal.c	\
	libimap-marshal.h	\
	imap_private.h	\
	siobuf.c	\
	siobuf.h	\
	util.c		\
	util.h

EXTRA_DIST = \
	libimap-marshal.list

INCLUDES= -I${top_builddir} -I${top_srcdir} -I${top_srcdir}/libbalsa \
	-I${top_srcdir}/libmutt -I${top_builddir}/libmutt \
	-I${top_srcdir}/libbalsa/imap \
	$(BALSA_CFLAGS)

AM_CFLAGS   = $(LIBIMAP_CFLAGS)
#-pedantic -ansi
#-Werror

#Signal marshallers
marshal_prefix = libimap
libimap-marshal.h: libimap-marshal.list
	( @GLIB_GENMARSHAL@ --prefix=$(marshal_prefix) libimap-marshal.list --header > libimap-marshal.tmp \
	&& mv libimap-marshal.tmp libimap-marshal.h ) \
	|| ( rm -f libimap-marshal.tmp && exit 1 )
libimap-marshal.c: libimap-marshal.h
	( echo "#include \"libimap-marshal.h\"" > libimap-marshal.tmp \
	&& @GLIB_GENMARSHAL@ --prefix=$(marshal_prefix) libimap-marshal.list --body >> libimap-marshal.tmp \
	&& mv libimap-marshal.tmp libimap-marshal.c ) \
	|| ( rm -f libimap-marshal.tmp && exit 1 )

#$(libimap_la_OBJECTS): libimap-marshal.h
$(libimap_a_OBJECTS): libimap-marshal.h