#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
#
# Copyright (c) 2014 Racktop Systems.
#
# CDDL HEADER END
#

#
# Copyright (c) 2001, 2010, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2012 OmniTI Computer Consulting, Inc. All rights reserved.
#

include ../Makefile.cmd

include $(SRC)/cmd/Makefile.cmd

<<<<<<< HEAD
SUBDIRS = \
	contrib/Sun/Solaris/BSM \
	contrib/Sun/Solaris/Intrs \
	contrib/Sun/Solaris/Kstat \
	contrib/Sun/Solaris/Lgrp \
	contrib/Sun/Solaris/Pg \
	contrib/Sun/Solaris/Project \
	contrib/Sun/Solaris/Task \
	contrib/Sun/Solaris/Utils

all :=		TARGET = all
install :=	TARGET = install
clean :=	TARGET = clean
clobber :=	TARGET = clobber

all install clean clobber: $(SUBDIRS)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)
=======
# PERL_LEGACY is versions of Perl still delivered through ON
PERL_VERSIONS = 5.16.1

all install test: $(PERL_VERSIONS)-$(MACH) .WAIT $(PERL_VERSIONS)-$(MACH64)

clean: FRC

clobber: clean
	$(RM) -r $(PERL_VERSIONS)-$(MACH) $(PERL_VERSIONS)-$(MACH64)

#
# Perl is not lint-clean.  Fake up a target.
#
lint:
	@ $(ECHO) "usr/src/cmd/perl is not lint-clean: skipping"
	@ $(TRUE)

$(PERL_VERSIONS)-$(MACH): FRC
	@ if [ ! -d $@ ]; then \
	    $(CP) -r skel $@; \
	fi
	@ cd $@; pwd; PERL_VERSION=$(PERL_VERSIONS) PERLARCH=$(MACH) $(MAKE) $(TARGET)

$(PERL_VERSIONS)-$(MACH64): FRC
	@ if [ ! -d $@ ]; then \
	    $(CP) -r skel $@; \
	fi
	@ cd $@; pwd; PERL_VERSION=$(PERL_VERSIONS) PERLARCH=$(MACH64) $(MAKE) $(TARGET)
>>>>>>> 0f5aa6827d31869b82ebc8fdec4d552bbe31d762

FRC:
