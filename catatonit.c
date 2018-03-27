/*
 * catatonit: a container init so simple it's effectively brain-dead
 * Copyright (C) 2018 SUSE LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "config.h"

static enum loglevel_t {
	LOG_FATAL = 0,
	LOG_ERROR = 1,
	LOG_WARN  = 2,
	LOG_INFO  = 3,
	LOG_DEBUG = 4,
} global_log_level = LOG_ERROR;

static void _log(enum loglevel_t level, char *fmt, ...)
{
	va_list ap;
	int old_errno = errno;
	char *level_str = "*";

	if (global_log_level < level)
		return;

	switch (level) {
	case LOG_FATAL:
		level_str = "FATAL";
		break;
	case LOG_ERROR:
	default:
		level_str = "ERROR";
		break;
	case LOG_WARN:
		level_str = "WARN";
		break;
	case LOG_INFO:
		level_str = "INFO";
		break;
	case LOG_DEBUG:
		level_str = "DEBUG";
		break;
	}

	fprintf(stderr, "%s (%s:%d): ", level_str, PROGRAM_NAME, getpid());

	va_start(ap, fmt);
	errno = old_errno;
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
	errno = old_errno;
}

static void usage(void)
{
	fprintf(stderr, "usage: %s [-hLV] [--] <progname> [<arguments>...]\n", PROGRAM_NAME);
}

static void help(void)
{
	usage();
	fprintf(stderr, "\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "  -h   Print this help page.\n");
	fprintf(stderr, "  -L   Print license information.\n");
	fprintf(stderr, "  -V   Print version information.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "The source code can be found at <%s>.\n", PROGRAM_URL);
	fprintf(stderr, "For bug reporting instructions, please see: <%s>\n", PROGRAM_BUGURL);
}

static void version(void)
{
	fprintf(stderr, "%s version %s\n", PROGRAM_NAME, PROGRAM_VERSION);
}

static void license(void)
{
	fprintf(stderr, "%s", PROGRAM_LICENSE);
}

#define LOG(level, ...) \
	do { _log(level, __VA_ARGS__); } while (0)

#define fatal(...) LOG(LOG_FATAL, __VA_ARGS__)
#define error(...) LOG(LOG_ERROR, __VA_ARGS__)
#define warn(...)  LOG(LOG_WARN,  __VA_ARGS__)
#define info(...)  LOG(LOG_INFO,  __VA_ARGS__)
#define debug(...) LOG(LOG_DEBUG, __VA_ARGS__)

#define bail(...) \
	do { error(__VA_ARGS__); exit(1); } while (0)
#define bail_usage(...) \
	do { error(__VA_ARGS__); usage(); exit(1); } while (0)

/*
 * Set of signals that the kernel sends us if *we* screwed something up. We
 * don't want to forward these to the child, as it will just confuse them. If
 * we get one of these, we let ourselves die rather than just carrying on.
 */
int kernel_signals[] = {SIGFPE, SIGILL, SIGSEGV, SIGSEGV, SIGBUS, SIGABRT, SIGTRAP, SIGSYS};

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof(*arr))

/*
 * Makes the current process a "foreground" process, by making it the leader of
 * a process group and session leader. It also updates the sigmask to include
 * signals that should be blocked.
 */
static int make_foreground(sigset_t *sigmask)
{
	/* Create a new process group. */
	if (setpgid(0, 0) < 0)
		bail("failed to create process group");
	pid_t pgrp = getpgrp();
	if (pgrp < 0)
		bail("failed to get new process group id");

	/*
	 * We open /dev/tty directly here. The reason for this (rather than just
	 * using STDIN_FILENO) is the the file descriptor could be duped over, but
	 * we still should become the controlling process.
	 */
	int ttyfd = open("/dev/tty", O_RDWR);
	if (ttyfd < 0) {
		info("using stdin as tty fd: could not open /dev/tty: %m");
		ttyfd = STDIN_FILENO;
	}

	/*
	 * Add TTY signals to ignored mask for pid1. This isn't strictly necessary,
	 * but we do it anyway to avoid pid1 being stopped inadvertently.
	 */
	if (sigaddset(sigmask, SIGTTOU) < 0)
		bail("failed to add SIGTTOU to pid1 mask");
	if (sigaddset(sigmask, SIGTTIN) < 0)
		bail("failed to add SIGTTIN to pid1 mask");

	/* Try to set ourselves as the owner of the terminal. */
	if (tcsetpgrp(ttyfd, pgrp) < 0) {
		switch (errno) {
		/* The fd wasn't a tty. This isn't a problem. */
		case ENOTTY:
		case EBADF:
			debug("setting foreground process failed: no tty present: %m");
			break;
		/* Can happen on lx-branded zones. Not a problem. */
		case ENXIO:
			debug("setting foreground process failed: no such device");
			break;
		/* Other errors are a problem. */
		default:
			bail("setting foreground process failed: %m");
			break;
		}
	}
	return 0;
}

/*
 * Spawn a child process with the given arguments and signal map and make it a
 * faux-pid1 by placing it in the foreground. This is the main process which
 * catatonit is going to be managing throughout its life.
 */
static int spawn_pid1(char *file, char **argv, sigset_t *sigmask)
{
	pid_t child = fork();
	if (child != 0) {
		if (child < 0)
			error("failed to fork child: %m");
		return child;
	}

	/*
	 * We are now in the child. Set up our sigmask, put ourselves in the
	 * foreground, and then finally exec (with the environment inherited).
	 */
	if (make_foreground(sigmask) < 0)
		bail("failed to become foreground: %m");
	if (sigprocmask(SIG_SETMASK, sigmask, NULL) < 0)
		bail("failed to reset sigmask: %m");

	execvpe(file, argv, __environ);
	bail("failed to exec pid1: %m");
}

/*
 * Handles any queued zombies which need to be reaped using waitpid(2). We
 * continually wait for child process deaths until none are reported (or we
 * have no children left).
 */
static int reap_zombies(void)
{
	for (;;) {
		int wstatus = 0;
		pid_t child = waitpid(-1, &wstatus, WNOHANG);
		if (child <= 0) {
			if (errno == ECHILD) {
				debug("got ECHILD: no children left to monitor");
				child = 0;
			}
			return child;
		}
		if (WIFEXITED(wstatus))
			debug("child process %d exited with code %d", child, WEXITSTATUS(wstatus));
		else if (WIFSIGNALED(wstatus))
			debug("child process %d exited due to signal %d", child, WTERMSIG(wstatus));
		else
			warn("observed unexpected status for process %d: %#x", child, wstatus);
	}
}

int main(int argc, char **argv)
{
	/* If CATATONIT_DEBUG is defined we change the global log level. */
	char *logstring = secure_getenv("CATATONIT_DEBUG");
	if (logstring != NULL)
		global_log_level = LOG_DEBUG;
	/* CATATONIT_LOG is reserved for future use. */
	if (secure_getenv("CATATONIT_LOG"))
		bail("CATATONIT_LOG is reserved for future use");

	/*
	 * Set up signal handling before *anything else*. We block *all* signals
	 * (except for signals that the kernel generates to try to kill us) since
	 * they will be read from the signalfd we set up. We also keep a copy of
	 * the original sigmask so we can re-set it on our faux-pid1.
	 */
	sigset_t init_sigmask, pid1_sigmask;
	if (sigfillset(&init_sigmask) < 0)
		bail("failed to fill init_sigmask: %m");
	int i;
	for (i = 0; i < ARRAY_LEN(kernel_signals); i++) {
		if (sigdelset(&init_sigmask, kernel_signals[i]) < 0)
			bail("failed to clear signal %d from init_sigmask: %m", kernel_signals[i]);
	}
	if (sigprocmask(SIG_SETMASK, &init_sigmask, &pid1_sigmask) < 0)
		bail("failed to block all signals: %m");

	int sfd = signalfd(-1, &init_sigmask, SFD_CLOEXEC);
	if (sfd < 0)
		bail("failed to create signalfd: %m");

	/*
	 * We need to support "--" as well as provide license information and so
	 * on. Aside from that we also need to update argv so it points at the
	 * first *pid1* argv argument rather than our own.
	 */
	int opt;
	while ((opt = getopt(argc, argv, "hLV")) != -1) {
		switch (opt) {
		case 'L':
			license();
			exit(0);
		case 'V':
			version();
			exit(0);
		case 'h':
			help();
			exit(0);
		default:
			usage();
			exit(1);
		}
	}
	argv += optind;
	argc -= optind;
	if (argc < 1)
		bail_usage("missing program name");

	/*
	 * If we aren't pid1, we have to set subreaper or bail. Otherwise zombies
	 * will collect on the host and that's just not a good idea. We don't just
	 * bail in all cases because users can run us in a container, but with the
	 * pid namespace shared with the host (though the benefit of using a
	 * container init is effectively zero in that instance).
	 */
	if (getpid() != 1) {
#if defined(PR_SET_CHILD_SUBREAPER)
		if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0)
			bail("failed to set child-reaper as non-pid1: %m");
#else
		bail("cannot run as non-pid1 without child-reaper support in kernel");
#endif
	}

	/* Spawn the faux-pid1. */
	pid_t pid1 = spawn_pid1(argv[0], argv, &pid1_sigmask);
	if (pid1 <= 0)
		bail("failed to spawn pid1: %m");

	/* One final check to make sure that it actually spawned. */
	if (kill(pid1, 0) < 0)
		bail("self-check that pid1 (%d) was spawned failed: %m", pid1);
	debug("pid1 (%d) spawned: %s", pid1, argv[0]);

	/*
	 * Wait for signals and process them as necessary. At this point we are no
	 * longer allowed to bail(), because if anything breaks it's ultimately our
	 * fault since a pid1 death will kill the container.
	 */
	int pid1_exitcode = -1;
	while (pid1_exitcode < 0) {
		/*
		 * Wait for a signal. read(2) will block here and we don't care about
		 * anything else, so no need for select(2) or epoll(2) or anything
		 * equivalently clever.
		 */
		struct signalfd_siginfo ssi = {0};

		int n = read(sfd, &ssi, sizeof(ssi));
		if (n != sizeof(ssi)) {
			if (n < 0)
				warn("signalfd read failed: %m");
			else
				warn("signalfd had %d-byte partial-read: %m", n);
			continue;
		}

		switch (ssi.ssi_signo) {
		/*
		 * Signals that we get sent if we are a background job in the current
		 * terminal (if it has TOSTOP set), which is possible since we make
		 * pid1 the foreground process. We just ignore them.
		 */
		case SIGTTOU: case SIGTTIN:
			debug("ignoring kernel attempting to stop us: tty has TOSTOP set");
			break;

		/* A child has died or a zombie has been re-parented to us. */
		/*
		 * TODO: We really should check ssi_pid, to see whether the sender was
		 * inside our pid namespace. This would help avoid cases where someone
		 * (foolishly) wants us to forward SIGCHLD to our pid1. Not sure why
		 * you'd ever want that, but no reason to not support it.
		 */
		case SIGCHLD:
			/*
			 * There is a special-case for our pid1. If the process exits we
			 * inherit its exit code, otherwise we assume an exit code of 127.
			 * This will cause us to exit immediately, since pid1 is now dead.
			 */
			if (ssi.ssi_pid == pid1) {
				/* Did it die from an exit(2)? */
				if (WIFEXITED(ssi.ssi_status))
					pid1_exitcode = WEXITSTATUS(ssi.ssi_status);
				/* What about from a signal? */
				else if (WIFSIGNALED(ssi.ssi_status))
					pid1_exitcode = 128 + WTERMSIG(ssi.ssi_status);
				/* Is the child actually dead? */
				else if (kill(pid1, 0) < 0)
					pid1_exitcode = 127;
				/* It hasn't died... */
				else
					warn("received signal from pid1 (%d) it is still alive", pid1);
			}
			if (reap_zombies() < 0)
				warn("problem occurred while reaping zombies: %m");
			break;

		/* A signal sent to us by a user which we must forward to pid1. */
		default:
			/* We just forward the signal to pid1. */
			if (kill(pid1, ssi.ssi_signo) < 0)
				warn("forwarding of signal %d to pid1 (%d) failed: %m", ssi.ssi_signo, pid1);
			break;
		}
	}
	return pid1_exitcode;
}
