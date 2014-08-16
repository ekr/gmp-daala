SRCS = gmp-daala.cpp
OBJS = $(SRCS:.cpp=.o)
CPPFLAGS = -Igmp-api -g
OGG_INCDIR=/opt/local/include
CPPFLAGS += -I$(OGG_INCDIR)
DAALA_SRCDIR = ../daala
DAALA_PLATFORM = unix
DAALA_LIBDIR = $(DAALA_SRCDIR)/$(DAALA_PLATFORM)
DAALA_LIBS = -L$(DAALA_LIBDIR) -ldaalaenc -ldaaladec -ldaalabase
DAALA_INCDIR = $(DAALA_SRCDIR)/include
CPPFLAGS += -I$(DAALA_INCDIR)
LINK = $(CXX) -shared
LIBNAME = libdaala.dylib
LIBS = $(DAALA_LIBS) 

$(LIBNAME): $(OBJS)
	$(LINK) $(OBJS) $(LIBS) -o $(LIBNAME)

gmp-bootstrap:
	if [ ! -d gmp-api ] ; then git clone https://github.com/mozilla/gmp-api gmp-api ; fi
	cd gmp-api && git fetch origin && git checkout $(GMP_API_BRANCH)

clean:
	rm -f $(OBJS) $(LIBNAME)



