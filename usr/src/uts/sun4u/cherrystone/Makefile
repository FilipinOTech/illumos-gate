#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# uts/sun4u/cherrystone/Makefile
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"%Z%%M%	%I%	%E% SMI"
#
#	This makefile drives the production of all Cherrystone system
#	dependent modules for the sun4u architecture.
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE = ../..

#
#	Include common rules.
#
include $(UTSBASE)/sun4u/cherrystone/Makefile.cherrystone

def		:=	TARGET= def
all		:=	TARGET= all
install		:=	TARGET= install
install_h	:=	TARGET= install_h
clean		:=	TARGET= clean
clobber		:=	TARGET= clobber
lint		:=	TARGET= lint
lintlib		:=	TARGET= lintlib
modlintlib	:=	TARGET= modlintlib
modlist		:=	TARGET= modlist
modlist		:=	NO_STATE= -K $$MODSTATE$$$$
clean.lint	:=	TARGET= clean.lint
check		:=	TARGET= check

#
#	Default build targets.
#
.KEEP_STATE:

def all clean.lint clean clobber modlist: $(CHERRYSTONE_KMODS)

modlintlib:	$(CHERRYSTONE_KMODS)

IMPLEMENTED_PLATFORM	= SUNW,Sun-Fire-480R

LINKED_PLATFORMS	= SUNW,Sun-Fire-V490

install: 	$(ROOT_CHERRYSTONE_DIR) $(USR_CHERRYSTONE_DIR) \
		$(USR_CHERRYSTONE_INC_DIR) \
		$(USR_CHERRYSTONE_SBIN_DIR) \
		$(USR_CHERRYSTONE_LIB_DIR) \
		$(LINKED_PLATFORMS:%=$(ROOT_PLAT_DIR)/%) \
		$(LINKED_PLATFORMS:%=$(USR_PLAT_DIR)/%/lib) \
		$(LINKED_PLATFORMS:%=$(USR_PLAT_DIR)/%/include) \
		$(LINKED_PLATFORMS:%=$(USR_PLAT_DIR)/%/sbin) \
		$(CHERRYSTONE_CRYPTO_LINKS) \
		.WAIT $(CHERRYSTONE_KMODS)

install_h check:	FRC
	@cd sys; pwd; $(MAKE) $(TARGET)

$(CHERRYSTONE_CRYPTO_LINKS): $(ROOT_CHERRYSTONE_CRYPTO_DIR_64)
	-$(RM) $(ROOT_CHERRYSTONE_CRYPTO_DIR_64)/$@;
	$(SYMLINK) $(ROOT_US3_CRYPTO_LINK)/$@ \
	    $(ROOT_CHERRYSTONE_CRYPTO_DIR_64)/$@

lint:		modlintlib

LINT_LIBS	 = $(LINT_LIB) \
		   -L$(CHERRYSTONE_LINT_LIB_DIR) \
		   -L$(LINT_LIB_DIR) $(LINT_KMODS:%=-l%) \
		       $(CLOSED_LINT_KMODS:%=-l%) \
		   -L$(SPARC_LIB_DIR) $(SPARC_LINTS:%=-l%)

lint.platmod:   modlintlib
	@-$(ECHO) "\nCherrystone Platform-dependent module: global crosschecks:"
	@-$(LINT) $(LINTFLAGS) $(LINT_LIBS) 2>&1 | $(LGREP.2)

$(CHERRYSTONE_KMODS):	FRC
	@cd $@; pwd; $(MAKE) $(NO_STATE) $(TARGET)

#
#
#	Include common targets.
#
include $(UTSBASE)/sun4u/cherrystone/Makefile.targ
