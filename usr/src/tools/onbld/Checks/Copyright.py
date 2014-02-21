#! /usr/bin/python
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
# Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
# Copyright 2008, 2010, Richard Lowe
# Copyright (c) 2012, 2014 by Delphix. All rights reserved.
#

# Make sure there is a copyright claim for the current year.

import time, re, sys

def err(stream, msg, fname):
	stream.write("%s: %s\n" % (fname, msg))

def is_delphix_copyright(line):
	return re.search(r'Copyright \(c\) (20\d\d, )?20\d\d by Delphix\. ' +
			r'All rights reserved\.$', line)

def is_copyright(line):
	return re.search(r'Copyright (?!\[yyyy\])', line)

def is_current_copyright(line):
	return re.search(r'Copyright.*\b%s\b' % time.strftime('%Y'), line)

def copyright(fh, filename=None, output=sys.stderr):
	ret = rights = goodrights = companyrights = 0

	if not filename:
		filename = fh.name

	for line in fh:
		if is_copyright(line):
			rights += 1
			if is_delphix_copyright(line):
				companyrights += 1
				if is_current_copyright(line):
					goodrights += 1
					break

	if rights == 0:
		err(output, "no copyright message found", filename)
		ret = 1
	elif companyrights == 0:
		err(output, "missing or mistyped Delphix copyright",
		    filename)
		ret = 1
	elif goodrights == 0:
		err(output, "no copyright claim for current year found",
		    filename)
		ret = 1

	return ret
