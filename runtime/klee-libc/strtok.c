// Taken from Musl:
// https://git.musl-libc.org/cgit/musl/plain/src/string/strtok.c

#include <string.h>

char *strtok(char *s, const char *sep)
{
	static char *p;
	if (!s && !(s = p)) return NULL;
	s += strspn(s, sep);
	if (!*s) return p = 0;
	p = s + strcspn(s, sep);
	if (*p) *p++ = 0;
	else p = 0;
	return s;
}
