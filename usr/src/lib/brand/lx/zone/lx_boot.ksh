#!/bin/ksh -p
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
# Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
# Copyright 2014, Joyent, Inc. All rights reserved.
#
# lx boot script.
#
# The arguments to this script are the zone name and the zonepath.
#

. /usr/lib/brand/shared/common.ksh

ZONENAME=$1
ZONEPATH=$2
ZONEROOT=$ZONEPATH/root

w_missing=$(gettext "Warning: \"%s\" is not installed in the global zone")

arch=`uname -p`
if [ "$arch" = "i386" ]; then
	ARCH32=i86
        ARCH64=amd64
else
        echo "Unsupported architecture: $arch" 
        exit 2
fi

#
# Run the lx_support boot hook.
#
/usr/lib/brand/lx/lx_support boot $ZONEPATH $ZONENAME
if (( $? != 0 )) ; then
        exit 1
fi

BRANDDIR=/native/usr/lib/brand/lx;
EXIT_CODE=1

# $1 is lx cmd, $2 is native cmd,
# the lx cmd path must have already be verified with safe_dir
setup_native_isaexeccmd() {
	cmd_name=$ZONEROOT/$1

	if [ -h $cmd_name -o \( -e $cmd_name -a ! -f $cmd_name \) ]; then
		logger -p daemon.err "dangerous zone cmd: $ZONENAME, $1" 
		return
	fi

	cat <<-DONE >$ZONEROOT/$1
	#!/bin/sh

	exec /native/usr/lib/brand/lx/lx_native \
	    /native/lib/ld.so.1 \
	    -e LD_NOENVIRON=1 \
	    -e LD_NOCONFIG=1 \
	    -e LD_PRELOAD_32=/native/usr/lib/brand/lx/lx_thunk.so.1 \
	    -e LD_LIBRARY_PATH_32="/native/lib:/native/usr/lib" \
	    $2 "\$@"
	DONE

	chmod 755 $ZONEROOT/$1
}

# $1 is lx cmd, $2 is native cmd, $3 is an optional inclusion in the script
# the lx cmd path must have already be verified with safe_dir
setup_native_cmd() {
	cmd_name=$ZONEROOT/$1

	if [ -h $cmd_name -o \( -e $cmd_name -a ! -f $cmd_name \) ]; then
		logger -p daemon.err "dangerous zone cmd: $ZONENAME, $1" 
		return
	fi

	cat <<-DONE >$ZONEROOT/$1
	#!/bin/sh

	LD_LIBRARY_PATH_32="/native/lib:/native/usr/lib"
	LD_PRELOAD=/native/usr/lib/brand/lx/lx_thunk.so.1
	LD_BIND_NOW=1
	export LD_LIBRARY_PATH LD_PRELOAD LD_BIND_NOW
	$3

	exec /native/usr/lib/brand/lx/lx_native $2 "\$@"
	DONE

	chmod 755 $ZONEROOT/$1
}

#
# Before we boot we validate and fix, if necessary, the required files within
# the zone.  These modifications can be lost if a patch or upgrade is applied
# within the zone, so we validate and fix the zone every time it boots.
#

#
# Determine the distro.
#
distro=""
if [[ -f $ZONEROOT/etc/redhat-release ]]; then
	distro="redhat"
elif [[ -f $ZONEROOT/etc/lsb-release ]]; then
	if egrep -s Ubuntu $ZONEROOT/etc/lsb-release; then
		distro="ubuntu"
	elif [[ -f $ZONEROOT/etc/debian_version ]]; then
		distro="debian"
	fi
elif [[ -f $ZONEROOT/etc/debian_version ]]; then
	distro="debian"
fi

[[ -z $distro ]] && fatal "Unsupported distribution!"

#
# BINARY REPLACEMENT
#
# This section of the boot script is responsible for replacing Linux
# binaries within the booting zone with native binaries.  This is a two-step
# process: First, the directory structure of the zone is validated to ensure
# that binary replacement will proceed safely.  Second, the Linux binaries
# are replaced with native binaries.
#
# Here's an example.  Suppose that you want to replace /usr/bin/zcat with the
# native /usr/bin/zcat binary.  Then you should do the following:
#
#	1.  Go to the section below labeled "STEP ONE" and add the following
#	    two lines:
#
#		safe_dir /usr
#		safe_dir /usr/bin
#
#	    These lines ensure that both /usr and /usr/bin are directories
#	    within the booting zone that can be safely accessed by the global
#	    zone.
#	2.  Go to the section below labeled "STEP TWO" and add the following
#	    line:
#
#		setup_native_cmd /usr/bin/zcat /native/usr/bin/zcat
#

#
# STEP ONE
#
# Validate that the zone filesystem looks like we expect it to.
#
safe_dir /bin
safe_dir /sbin
safe_dir /etc
safe_dir /etc/init
safe_dir /etc/update-motd.d

#
# STEP TWO
#
# Replace Linux binaries with native binaries.
#
setup_native_isaexeccmd /sbin/ifconfig /native/sbin/ifconfig
setup_native_isaexeccmd /sbin/dladm /native/usr/sbin/dladm
setup_native_isaexeccmd /sbin/route /native/usr/sbin/route
setup_native_cmd /sbin/ipmgmtd /native/lib/inet/ipmgmtd \
	"export SMF_FMRI=\"svc:/network/ip-interface-management:default\""
setup_native_cmd /bin/netstat /native/usr/bin/netstat

#
# STEP THREE
#
# Perform distro-specific customization.
#
. $(dirname $0)/lx_boot_zone_${distro}

exit 0
