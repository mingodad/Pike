#!/usr/bin/env pike
#pragma strict_types
/* $Id: prime_table.pike,v 1.2 2002/04/29 21:24:07 mikael%brandstrom.org Exp $
 *
 * Generates a table of primes.
 * Used when cross-compiling.
 *
 * Henrik Grubbström 2000-08-01
 */
int main(int argc, array(string) argv)
{
  if (argc != 2) {
    werror("Bad number of arguments!\n");
    exit(1);
  }
  int count = (int)argv[1];
  write(sprintf("/* Automatically generated by\n"
		" * $Id: prime_table.pike,v 1.2 2002/04/29 21:24:07 mikael%brandstrom.org Exp $\n"
		" *%{ %s%}\n"
		" * Do not edit.\n"
		" */\n"
		"\n"
		"#define NUMBER_OF_PRIMES %d\n"
		"\n"
		"const unsigned long primes[NUMBER_OF_PRIMES] = {",
		argv, count));
  int prime = 1;
  for (int i=0; i < count; i++) {
    prime = (prime+1)->next_prime();
    if (!(i%10)) {
      write(sprintf("\n   %d,", prime));
    } else {
      write(sprintf(" %d,", prime));
    }
  }
  write("\n};\n"
	"\n");
  return 0;
}
