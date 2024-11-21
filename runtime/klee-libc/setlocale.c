extern int __setlocale_fails(void);

static char *__locale = "C";

/// 
// This model underapproximates!
const char *setlocale(int category, const char *locale) {
   if (!locale) {
	return __locale;
   }

  if (__setlocale_fails())
      return 0;

  return __locale;
}
