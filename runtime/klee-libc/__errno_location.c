extern int __klee_errno;
int *__errno_location(void)
{
	/* we don't support multi-threaded programs,
	 * so we can have just this one errno */
	return &__klee_errno;
}
