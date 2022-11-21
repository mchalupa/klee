#include <unistd.h>

extern char nondet_char(void);
extern unsigned nondet_uint(void);
extern void klee_assume(int);
extern void klee_make_symbolic(void *, size_t, const char *);

static char dummy_env[20];

char *getenv(const char *name) {
	(void) name;

	if (nondet_char())
		return ((char *) 0);

	klee_make_symbolic(dummy_env, sizeof(dummy_env), "dummy_env");

	unsigned int idx = nondet_uint();
	klee_assume(idx < sizeof(dummy_env));
	dummy_env[idx] = '\0';

	return dummy_env;
}
