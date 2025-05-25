lib_LTLIBRARIES =
bin_PROGRAMS =

if DEBUG
DBGFLAGS = -ggdb -DDEBUG -gdwarf-5
else
DBGFLAGS = -g -DNDEBUG
endif

SWIG_SOURCES = pyext/swsscommon.i
SWIG_FLAG = -Wall -c++ -python -keyword
if ARCH64
SWIG_FLAG += -DSWIGWORDSIZE64
endif

CLEANFILES =

# Define SUBDIRS if not already present or add 'doc' to it
# If SUBDIRS is already defined, this will need careful merging.
# Assuming SUBDIRS might not be defined or is simple.
SUBDIRS = . common pyext sonic-db-cli tests doc

include common/Makefile.am
include pyext/Makefile.am
include sonic-db-cli/Makefile.am
include tests/Makefile.am
# doc/Makefile.am will be processed due to 'doc' in SUBDIRS


ACLOCAL_AMFLAGS = -I m4
