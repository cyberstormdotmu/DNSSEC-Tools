/*
 * Copyright (c) 2004 by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1996,1999 by Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "validator-config.h"

#include <sys/types.h>
#include <arpa/nameser.h>

#include "validator/resolver.h"
#include "res_support.h"

/*
 * Public. 
 */
#ifdef NETBSD
#define NS_GETPUT16_TYPE u_int16_t
#define NS_GETPUT32_TYPE u_int32_t
#else
#define NS_GETPUT16_TYPE u_int
#define NS_GETPUT32_TYPE u_long
#endif

NS_GETPUT16_TYPE
ns_get16(const u_char * src)
{
    NS_GETPUT16_TYPE dst;

    RES_GET16(dst, src);
    return (dst);
}

NS_GETPUT32_TYPE
ns_get32(const u_char * src)
{
    NS_GETPUT32_TYPE dst;

    RES_GET32(dst, src);
    return (dst);
}

void
ns_put16(NS_GETPUT16_TYPE src, u_char * dst)
{
    RES_PUT16(src, dst);
}

void
ns_put32(NS_GETPUT32_TYPE src, u_char * dst)
{
    RES_PUT32(src, dst);
}
