#include <errno.h>

static const char *__domain = "";
static const char *__dirname = "";


extern int __textdomain_fail(void);
extern int __gettext_fail(void);

char * textdomain (const char * domainname) {
	if (__textdomain_fail()) {
		errno = ENOMEM;
		return 0;
	}

	if (domainname)
		__domain = domainname;

	return (char*)__domain;
}


// This is underapprox
char *bindtextdomain (const char *domainname, const char *dirname) {
	if (__textdomain_fail()) {
		errno = ENOMEM;
		return 0;
	}

	if (dirname)
		__dirname = dirname;

	return __dirname;
}

static const char * __dummy = "dummy_gettext";
char *gettext(const char *msgid) {
	if (__gettext_fail()) {
		return (char*)__dummy;
	}
	return (char*)msgid;
}
