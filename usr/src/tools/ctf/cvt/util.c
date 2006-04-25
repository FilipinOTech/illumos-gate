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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * Utility functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libelf.h>
#include <gelf.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/param.h>

#include "ctftools.h"
#include "memory.h"

static void (*terminate_cleanup)() = NULL;

/* returns 1 if s1 == s2, 0 otherwise */
int
streq(char *s1, char *s2)
{
	if (s1 == NULL) {
		if (s2 != NULL)
			return (0);
	} else if (s2 == NULL)
		return (0);
	else if (strcmp(s1, s2) != 0)
		return (0);

	return (1);
}

int
findelfsecidx(Elf *elf, char *tofind)
{
	Elf_Scn *scn = NULL;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;

	if (gelf_getehdr(elf, &ehdr) == NULL) {
		terminate("gelf_getehdr: %s\n", elf_errmsg(elf_errno()));
	}

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		char *name;

		if (gelf_getshdr(scn, &shdr) == NULL ||
		    (name = elf_strptr(elf, ehdr.e_shstrndx,
		    (size_t)shdr.sh_name)) == NULL) {
			terminate("gelf_getehdr: %s\n",
			    elf_errmsg(elf_errno()));
		}

		if (strcmp(name, tofind) == 0)
			return (elf_ndxscn(scn));
	}

	return (-1);
}

/*PRINTFLIKE2*/
static void
whine(char *type, char *format, va_list ap)
{
	int error = errno;

	fprintf(stderr, "%s: %s: ", type, progname);
	vfprintf(stderr, format, ap);

	if (format[strlen(format) - 1] != '\n')
		fprintf(stderr, ": %s\n", strerror(error));
}

void
vaterminate(char *format, va_list ap)
{
	whine("ERROR", format, ap);
}

void
set_terminate_cleanup(void (*cleanup)())
{
	terminate_cleanup = cleanup;
}

/*PRINTFLIKE1*/
void
terminate(char *format, ...)
{
	if (format) {
		va_list ap;

		va_start(ap, format);
		whine("ERROR", format, ap);
		va_end(ap);
	}

	if (terminate_cleanup)
		terminate_cleanup();

	exit(1);
}

/*PRINTFLIKE1*/
void
warning(char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	whine("WARNING", format, ap);
	va_end(ap);

	if (debug_level >= 3)
		terminate("Termination due to warning\n");
}

/*PRINTFLIKE2*/
void
vadebug(int level, char *format, va_list ap)
{
	if (level > debug_level)
		return;

	(void) fprintf(DEBUG_STREAM, "DEBUG: ");
	(void) vfprintf(DEBUG_STREAM, format, ap);
	fflush(DEBUG_STREAM);
}

/*PRINTFLIKE2*/
void
debug(int level, char *format, ...)
{
	va_list ap;

	if (level > debug_level)
		return;

	va_start(ap, format);
	(void) vadebug(level, format, ap);
	va_end(ap);
}

char *
mktmpname(const char *origname, const char *suffix)
{
	char *newname;

	newname = xmalloc(strlen(origname) + strlen(suffix) + 1);
	(void) strcpy(newname, origname);
	(void) strcat(newname, suffix);
	return (newname);
}

/*PRINTFLIKE2*/
void
elfterminate(const char *file, const char *fmt, ...)
{
	static char msgbuf[BUFSIZ];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msgbuf, sizeof (msgbuf), fmt, ap);
	va_end(ap);

	terminate("%s: %s: %s\n", file, msgbuf, elf_errmsg(elf_errno()));
}

const char *
tdesc_name(tdesc_t *tdp)
{
	return (tdp->t_name == NULL ? "(anon)" : tdp->t_name);
}
