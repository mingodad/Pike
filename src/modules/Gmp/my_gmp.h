/* $Id: my_gmp.h,v 1.10 2001/08/13 23:33:48 hubbe Exp $
 *
 * These functions or something similar will hopefully be included
 * with Gmp-2.1 .
 */

#ifndef MY_GMP_H_INCLUDED
#define MY_GMP_H_INCLUDED

/* Kludge for some compilers only defining __STDC__ in strict mode,
 * which leads to <gmp.h> using the wrong token concat method.
 */
#if !defined(__STDC__) && defined(HAVE_ANSI_CONCAT) && defined(PIKE_MPN_PREFIX)
#define PIKE_LOW_MPN_CONCAT(x,y)	x##y
#define PIKE_MPN_CONCAT(x,y)	PIKE_LOW_MPN_CONCAT(x,y)
#define __MPN(x)	PIKE_MPN_CONCAT(PIKE_MPN_PREFIX,x)
#endif /* !__STDC__ && HAVE_ANSI_CONCAT && PIKE_MPN_PREFIX */

#undef _PROTO
#define _PROTO(x) x

#ifdef USE_GMP2
#include <gmp2/gmp.h>
#else /* !USE_GMP2 */
#include <gmp.h>
#endif /* USE_GMP2 */

struct pike_string;

/* MPZ protos */

unsigned long mpz_small_factor(mpz_t n, int limit);

void mpz_next_prime(mpz_t p, mpz_t n, int count, int prime_limit);
void my_mpz_xor _PROTO ((mpz_ptr, mpz_srcptr, mpz_srcptr));
void get_mpz_from_digits(MP_INT *tmp,
			 struct pike_string *digits,
			 int base);
void get_new_mpz(MP_INT *tmp, struct svalue *s);
MP_INT *debug_get_mpz(struct svalue *s, int throw_error);
void mpzmod_reduce(struct object *o);
struct pike_string *low_get_mpz_digits(MP_INT *mpz, int base);

extern struct program *mpzmod_program;
#ifdef AUTO_BIGNUM
extern struct program *bignum_program;
#endif

#ifdef DEBUG_MALLOC
#define get_mpz(X,Y) \
 (debug_get_mpz((X),(Y)),( (X)->type==T_OBJECT? debug_malloc_touch((X)->u.object) :0 ),debug_get_mpz((X),(Y)))
#else
#define get_mpz debug_get_mpz 
#endif


#define OBTOMPZ(o) ((MP_INT *)(o->storage))


/* MPQ protos */
void pike_init_mpq_module(void);
void pike_exit_mpq_module(void);

#endif /* MY_GMP_H_INCLUDED */
