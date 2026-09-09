/*
 * child.c -- the program that gets loaded by exec().
 *
 * This is a completely separate binary from the parent. That separation is the
 * whole point of the demo: fork() makes a new *process*, exec() replaces the
 * *program* running inside it. Two distinct operations that people constantly
 * mash together into one verb.
 *
 * Usage: ./child <seconds>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	/*
	 * NOTE(learn): argv here did NOT come from the shell -- the parent built
	 * it by hand and handed it to execv(). By convention argv[0] is the
	 * program name, but that is only a convention: the caller of exec()
	 * chooses it and can lie. This is exactly how a process can show up in
	 * `ps` under a name that has nothing to do with the binary on disk.
	 */
	if (argc != 2) {
		fprintf(stderr, "usage: %s <seconds>\n",
		        (argc > 0 && argv[0]) ? argv[0] : "child");
		return EXIT_FAILURE;
	}

	/*
	 * NOTE(learn): atoi() cannot report failure -- atoi("banana") is 0 and
	 * atoi("99999999999999") is undefined behavior. strtol() with an errno
	 * reset and an end-pointer check is the version that actually tells you
	 * whether the parse succeeded. Cheap habit, worth having.
	 */
	errno = 0;
	char *end = NULL;
	long secs = strtol(argv[1], &end, 10);
	if (errno != 0 || end == argv[1] || *end != '\0' || secs < 0 || secs > 60) {
		fprintf(stderr, "child : bad <seconds> argument: \"%s\"\n", argv[1]);
		return EXIT_FAILURE;
	}

	/*
	 * getpid() is the child's own pid; getppid() is the parent's. The parent
	 * prints the value fork() handed it -- the two must agree. If they ever
	 * disagreed, something in your mental model is wrong.
	 */
	printf("child : pid=%ld ppid=%ld -- new program image, sleeping %ld second(s)\n",
	       (long)getpid(), (long)getppid(), secs);

	/*
	 * NOTE(learn): flush before a long blocking call. stdout to a terminal is
	 * line-buffered so the \n above already pushed it out -- but pipe the
	 * program into `cat` and stdout becomes block-buffered, and this message
	 * would sit invisible in a buffer for the whole sleep. Try it:
	 *     ./forkexec | cat
	 * The buffering mode depends on what stdout is attached to, which is a
	 * runtime property you do not control.
	 */
	fflush(stdout);

	/*
	 * NOTE(learn): sleep() is not guaranteed to sleep the full time. A signal
	 * delivered mid-sleep cuts it short, and sleep() returns the number of
	 * seconds still owed. Ignoring that return value is a real bug class:
	 * "why did my 6-second delay only take 2 seconds?" This loop finishes the
	 * job. Same idea as the EINTR retry loop around waitpid() in the parent --
	 * on POSIX, "it got interrupted" is a normal outcome, not an error.
	 */
	unsigned int remaining = (unsigned int)secs;
	while (remaining > 0)
		remaining = sleep(remaining);

	printf("child : pid=%ld -- done sleeping, exiting with status %ld\n",
	       (long)getpid(), secs);

	/*
	 * NOTE(learn): returning from main() is equivalent to exit() -- it runs
	 * atexit handlers and flushes stdio. Only the low 8 bits survive into the
	 * parent's wait status, so a status of 256 arrives as 0. Keep exit codes
	 * in 0..255, and remember 0 conventionally means success.
	 */
	return (int)secs;
}
