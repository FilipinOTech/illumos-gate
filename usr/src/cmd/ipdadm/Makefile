#
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
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
# Copyright (c) 2012 Joyent, Inc.  All rights reserved.
#

PROG= ipdadm
OBJS = ipdadm.o
SRCS = $(OBJS:%.o=../%.c)


include ../Makefile.cmd
include ../Makefile.ctf

CLEANFILES += $(OBJS)
CFLAGS += $(CCVERBOSE)
LDLIBS += -lipd

.KEEP_STATE:

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
	$(POST_PROCESS)
	$(CTFCONVERT_O)

clean:
	-$(RM) $(CLEANFILES)

lint: lint_PROG

%.o: ../%.c
	$(COMPILE.c) $<
	$(POST_PROCESS_O)

install: $(PROG) $(ROOTUSRSBINPROG)

include ../Makefile.targ
