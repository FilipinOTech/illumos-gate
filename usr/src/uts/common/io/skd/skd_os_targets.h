/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2013 STEC, Inc.  All rights reserved.
 */

#ifndef _SKD_OS_TARGETS_H
#define	_SKD_OS_TARGETS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Target OS:
 *
 * Solaris 11.1		- define SOLARIS11_1 and SOLARIS11 only
 *
 * Solaris 11.0		- define SOLARIS11
 * Illumos 		- define SOLARIS11
 *
 * Solaris 10		- do not define SOLARIS11_1 nor SOLARIS11 nor NEX31
 *
 * Nexenta 3.1		- define SOLARIS11 and NEX31
 *
 */

/* #define	SOLARIS11_1	1 */ /* Solaris 11.1 */
/* #define	NEX31		1 */ /* Nexenta 3.1 */
#define	SOLARIS11	1 /* For Solaris 11.0 and Illumos. */

#ifdef	__cplusplus
}
#endif

#endif /* _SKD_OS_TARGETS_H */
