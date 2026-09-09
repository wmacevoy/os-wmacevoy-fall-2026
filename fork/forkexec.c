/*
 * forkexec.c -- minimal, correct fork()/exec()/wait() demo.
 *
 * The lifecycle every UNIX process launch follows:
 *
 *     fork()    duplicate the calling process           -> now 2 processes
 *     exec()    replace this process's program image    -> still 2 processes
 *     wait()    parent collects the child's exit status -> back to 1
 *
 * The shell does exactly this every time you type a command. `&` just means
 * the shell skips (defers) the wait.
 *
 * Usage: ./forkexec [path-to-child]
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIN_SLEEP 1
#define MAX_SLEEP 6

/* Exit status a shell uses for "command found but could not be executed". */
#define EXIT_EXEC_FAILED 126

int main(int argc, char *argv[])
{
	const char *child_path = (argc > 1) ? argv[1] : "./child";

	/*
	 * NOTE(learn): seeding with time(NULL) alone gives every process started
	 * in the same second an identical "random" sequence. XOR-ing in the pid
	 * de-correlates them. rand() is fine for a demo and wrong for anything
	 * security-adjacent -- know which situation you are in.
	 */
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	int secs = MIN_SLEEP + rand() % (MAX_SLEEP - MIN_SLEEP + 1);

	/* exec() takes strings, so the duration has to be rendered as text. */
	char secs_arg[16];
	snprintf(secs_arg, sizeof secs_arg, "%d", secs);

	printf("parent: pid=%ld -- forking; child will sleep %d second(s)\n",
	       (long)getpid(), secs);

	/*
	 * NOTE(learn): THE classic fork gotcha, stated precisely -- the sloppy
	 * version of this warning is everywhere and it is subtly wrong.
	 *
	 * fork() copies the address space, and stdio's buffer lives in the address
	 * space. So anything buffered-but-unwritten at fork() time now exists
	 * twice, and if BOTH processes eventually flush, the text is emitted
	 * twice. It hides on a terminal (line-buffered, so the \n already pushed
	 * it out) and shows up the moment stdout is a file or pipe
	 * (block-buffered).
	 *
	 * But note what saves us here even without this flush: a successful
	 * exec() DISCARDS the address space, buffered bytes included. They are
	 * thrown away, never flushed. So in a plain fork+exec the duplicate does
	 * not appear -- the trap is really about fork() WITHOUT exec(), and about
	 * the failure path just below, where exec() did not happen and the child
	 * still owns a copy of our buffer.
	 *
	 * Verified, not assumed: removing this line and piping to `cat` prints
	 * nothing twice. Removing it *and* changing the _exit() below to exit()
	 * does duplicate the line above. See `make demo-dup`.
	 *
	 * Keep the flush anyway. It costs nothing and it stops being a question
	 * you have to re-reason about every time the code changes.
	 */
	fflush(NULL);

	pid_t pid = fork();

	if (pid < 0) {
		/* No child was created. Common causes: RLIMIT_NPROC, out of memory. */
		perror("parent: fork");
		return EXIT_FAILURE;
	}

	/*
	 * NOTE(learn): fork() is the function that returns twice -- once in each
	 * process, from the same source line, with different values:
	 *     < 0  error, no child exists (only the caller returns)
	 *     0    you are the child
	 *     > 0  you are the parent; the value is the child's pid
	 * The child cannot learn its own pid from fork() (it gets 0); it calls
	 * getpid(). Which process runs first after this point is NOT specified --
	 * never write code whose correctness depends on that ordering.
	 */
	if (pid == 0) {
		/* ---------------- child ---------------- */

		/*
		 * NOTE(learn): execv() replaces this process's code, data, heap and
		 * stack with a new program, but KEEPS the process: same pid, same
		 * parent, same open file descriptors (which is how the child inherits
		 * our stdout without anyone passing it along). No new process is
		 * created here -- that already happened at fork().
		 *
		 * The cast is the well-known ugliness of the exec() prototype: it
		 * wants char *const[] even though it never modifies the strings. It
		 * is a historical wart, not a hint that you should write to them.
		 */
		char *const child_argv[] = { (char *)child_path, secs_arg, NULL };
		execv(child_path, child_argv);

		/*
		 * NOTE(learn): reaching this line means execv() FAILED. On success
		 * there is no "after" -- the code that would have run was overwritten.
		 * An unchecked exec() failure is nasty precisely because the process
		 * survives and keeps running the parent's code as a second parent.
		 *
		 * And use _exit(), not exit(). This is the load-bearing one: exec()
		 * did not happen, so this process still holds a COPY of the parent's
		 * stdio buffers, and exit() would flush them -- reprinting the
		 * parent's pending output as if the child had said it. _exit() goes
		 * straight to the kernel and skips all userspace cleanup (no atexit
		 * handlers, no stdio flush). In any child that is bailing out without
		 * a successful exec, that is what you want.
		 */
		fprintf(stderr, "child : execv(\"%s\") failed: %s\n",
		        child_path, strerror(errno));
		_exit(EXIT_EXEC_FAILED);
	}

	/* ---------------- parent ---------------- */

	printf("parent: fork() returned %ld -- that is the child's pid; waiting\n",
	       (long)pid);
	fflush(stdout);

	/*
	 * NOTE(learn): until the parent waits, a finished child stays in the
	 * process table as a zombie -- dead, but holding its exit status for
	 * someone to read. wait() is how the status is collected and the entry
	 * freed. A parent that loops forever without waiting leaks zombies; a
	 * parent that dies first hands its orphans to init/launchd, which reaps
	 * them.
	 *
	 * waitpid() targets one specific child; wait() takes whichever finishes
	 * first. The retry loop handles EINTR -- a signal arriving while we are
	 * blocked makes waitpid() return -1/EINTR without the child having
	 * exited, and "retry" is the correct response, not "fail".
	 */
	int status = 0;
	pid_t waited;
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);

	if (waited < 0) {
		perror("parent: waitpid");
		return EXIT_FAILURE;
	}

	/*
	 * NOTE(learn): `status` is NOT the exit code. It is a packed word encoding
	 * *how* the child ended -- exited, killed by a signal, stopped. Always
	 * decode it with the W* macros; never compare it to a number directly.
	 * Try `kill -9 <child pid>` mid-sleep from another terminal to make the
	 * WIFSIGNALED branch fire.
	 */
	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);
		printf("parent: child %ld exited normally, status %d\n",
		       (long)waited, code);
		/*
		 * The child was written to exit with the number of seconds it slept,
		 * so a matching status is evidence it really ran to completion. A
		 * mismatch (e.g. 126) means it never got that far -- exec failed.
		 */
		if (code == secs)
			printf("parent: status matches the %d second(s) we requested\n",
			       secs);
		else
			printf("parent: expected %d -- the child did not run to completion\n",
			       secs);
		printf("parent: pid=%ld -- exiting with the child's status\n",
		       (long)getpid());
		return code;
	}

	if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		printf("parent: child %ld killed by signal %d (%s)\n",
		       (long)waited, sig, strsignal(sig));
		/* Shell convention for "died by signal N". */
		return 128 + sig;
	}

	printf("parent: child %ld ended in an unexpected way (status word 0x%x)\n",
	       (long)waited, (unsigned)status);
	return EXIT_FAILURE;
}
