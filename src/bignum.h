/*
|| This file is part of Pike. For copyright information see COPYRIGHT.
|| Pike is distributed under GPL, LGPL and MPL. See the file COPYING
|| for more information.
|| $Id: bignum.h,v 1.21 2003/03/28 23:39:01 mast Exp $
*/

#include "global.h"

#ifndef BIGNUM_H
#define BIGNUM_H

/* Note: These functions assume some properties of the CPU. */

#define INT_TYPE_SIGN(x)             ((x) < 0)

#if HAVE_NICE_FPU_DIVISION
#define INT_TYPE_MUL_OVERFLOW(a, b)  ((b) && ((a)*(b))/(b) != (a))
#else
#define INT_TYPE_MUL_OVERFLOW(a, b)                                        \
        ((b) && (INT_TYPE_DIV_OVERFLOW(a, b) || ((a)*(b))/(b) != (a)))
#endif

#define INT_TYPE_DIV_OVERFLOW(a, b)  (INT_TYPE_NEG_OVERFLOW(a) && (b) == -1)

#define INT_TYPE_NEG_OVERFLOW(x)     ((x) && (x) == -(x))

#define INT_TYPE_ADD_OVERFLOW(a, b)                                        \
        (INT_TYPE_SIGN(a) == INT_TYPE_SIGN(b) &&                           \
	 INT_TYPE_SIGN(a) != INT_TYPE_SIGN((a)+(b)))

#define INT_TYPE_SUB_OVERFLOW(a, b)                                        \
        (INT_TYPE_SIGN(a) != INT_TYPE_SIGN(b) &&                           \
	 INT_TYPE_SIGN(a) != INT_TYPE_SIGN((a)-(b)))

#define INT_TYPE_LSH_OVERFLOW(a, b)                                        \
        ((((INT_TYPE)sizeof(INT_TYPE))*CHAR_BIT <= (b) && (a)) ||          \
	 (((a)<<(b))>>(b)) != (a))

/* Note: If this gives overflow, set the result to zero. */
#define INT_TYPE_RSH_OVERFLOW(a, b)                                        \
        (((INT_TYPE)sizeof(INT_TYPE))*CHAR_BIT <= (b) && (a))

#ifdef AUTO_BIGNUM

/* Prototypes begin here */
struct program *get_auto_bignum_program(void);
struct program *get_auto_bignum_program_or_zero(void);
void init_auto_bignum(void);
void exit_auto_bignum(void);
void convert_stack_top_to_bignum(void);
void convert_stack_top_with_base_to_bignum(void);
int is_bignum_object(struct object *o);
int is_bignum_object_in_svalue(struct svalue *sv);
struct object *make_bignum_object(void);
struct object *bignum_from_svalue(struct svalue *s);
struct pike_string *string_from_bignum(struct object *o, int base);
void convert_svalue_to_bignum(struct svalue *s);

#ifdef INT64
PMOD_EXPORT void (*push_int64)(INT64 i);
PMOD_EXPORT int (*int64_from_bignum) (INT64 *i, struct object *bignum);
#else
#define push_int64(i) push_int((INT_TYPE)(i))
#define int64_from_bignum(I,BIGNUM)	0
#endif /* INT64 */
/* Prototypes end here */

#else

#define push_int64(i) push_int((INT_TYPE)(i))
#define int64_from_bignum(I,BIGNUM)	0

#endif /* AUTO_BIGNUM */

#endif /* BIGNUM_H */
