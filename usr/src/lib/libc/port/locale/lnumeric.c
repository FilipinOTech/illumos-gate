/*
 * Copyright 2010 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2000, 2001 Alexey Zelkin <phantom@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "lint.h"
#include <limits.h>
#include "ldpart.h"
#include "lnumeric.h"
#include "../i18n/_locale.h"

extern int __nlocale_changed;
extern const char *__fix_locale_grouping_str(const char *);

#define	LCNUMERIC_SIZE (sizeof (struct lc_numeric_T) / sizeof (char *))

static char	numempty[] = { CHAR_MAX, '\0' };

static const struct lc_numeric_T _C_numeric_locale = {
	".",		/* decimal_point */
	"",		/* thousands_sep */
	numempty	/* grouping */
};

static struct lc_numeric_T _numeric_locale;
static int	_numeric_using_locale;
static char	*_numeric_locale_buf;

int
__numeric_load_locale(const char *name)
{
	const struct lc_numeric_T *leg = &_C_numeric_locale;

	int ret;

	ret = __part_load_locale(name, &_numeric_using_locale,
	    &_numeric_locale_buf, "LC_NUMERIC", LCNUMERIC_SIZE, LCNUMERIC_SIZE,
	    (const char **)&_numeric_locale);
	if (ret == _LDP_ERROR)
		return (_LDP_ERROR);

	__nlocale_changed = 1;
	if (ret == _LDP_LOADED) {
		/* Can't be empty according to C99 */
		if (*_numeric_locale.decimal_point == '\0')
			_numeric_locale.decimal_point =
			    _C_numeric_locale.decimal_point;
		_numeric_locale.grouping =
		    __fix_locale_grouping_str(_numeric_locale.grouping);
		leg = (const struct lc_numeric_T *)&_numeric_locale;
	}
	/* This is Solaris legacy, required for ABI compatability */
	_numeric[0] = *leg->decimal_point;
	_numeric[1] = *leg->grouping;
	return (ret);
}

struct lc_numeric_T *
__get_current_numeric_locale(void)
{
	return (_numeric_using_locale ? &_numeric_locale :
	    (struct lc_numeric_T *)&_C_numeric_locale);
}