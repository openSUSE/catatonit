// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * catatonit: a container init so simple it's effectively brain-dead
 * Copyright (C) 2018-2023 SUSE LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>
#include <dirent.h>

#ifdef HAVE_LINUX_CLOSE_RANGE_H
# include <linux/close_range.h>
#else
# include <sys/syscall.h>
#endif

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
	fprintf(stderr, "usage: %s [-ghLPV] [--] <progname> [<arguments>...]\n", PROGRAM_NAME);
}

static void help(void)
{
	usage();
	fprintf(stderr, "\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "  -g              Forward signals to pid1's process group.\n");
	fprintf(stderr, "  -h              Print this help page.\n");
	fprintf(stderr, "  -L              Print license information.\n");
	fprintf(stderr, "  -P              Run in pause mode (no program is run and quit on SIGINT).\n");
	fprintf(stderr, "  -V, --version   Print version information.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "The source code can be found at <%s>.\n", PROGRAM_URL);
	fprintf(stderr, "For bug reporting instructions, please see: <%s>.\n", PROGRAM_BUGURL);
}

static void version(void)
{
	// The name is intentional to make `docker-info` happy: docker is hard-coded
	// against `tini`. This is an (unfortunate) hack to make it work nicely with
	// catatonit.
	fprintf(stdout, "tini version %s_%s\n", PROGRAM_VERSION, PROGRAM_NAME);
}

static void license(void)
{
	fprintf(stdout, "%s", PROGRAM_LICENSE);
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
int kernel_signals[] = {SIGFPE, SIGILL, SIGSEGV, SIGBUS, SIGABRT, SIGTRAP, SIGSYS};

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof(*arr))

#ifndef HAVE_CLOSE_RANGE
static int close_range(unsigned int fd, unsigned int max_fd, unsigned int flags)
{
# ifdef __NR_close_range
  return (int) syscall(__NR_close_range, fd, max_fd, flags);
# else
  errno = ENOSYS;
  return -1;
# endif
}
#endif

/*
 * Close every fd >= n that is different from exclude_fd using close_range.
 */
static int close_range_fds_ge_than(int n, int exclude_fd)
{
	int r, saved_errno = 0;

	/* exclude_fd is not in the [n, UINT_MAX] range.  */
	if (exclude_fd < n)
		return close_range(n, UINT_MAX, 0);

	/* exclude_fd is the first fd in the [n, UINT_MAX] range.  */
	if (exclude_fd == n)
		return close_range(n + 1, UINT_MAX, 0);

	/* exclude_fd is between n and UINT_MAX.  */
	errno = 0;
	r = close_range(n, exclude_fd - 1, 0);
	/*
	 * attempt to close as many FDs as possible but return an error
	 * if the close_range() failed.
	 */
	if (exclude_fd < UINT_MAX) {
		saved_errno = errno;
		r = close_range(exclude_fd + 1, UINT_MAX, 0);
		/* If the previous call failed, restore errno.  */
		if (saved_errno != 0) {
			r = -1;
			errno = saved_errno;
		}
	}
	return r;
}

/*
 * Close every fd >= n that is different from exclude_fd.
 */
static int close_fds_ge_than(int n, int exclude_fd)
{
	struct dirent *next;
	int failures = 0;
	DIR *dir;
	int fd;
	int r;

	if (close_range_fds_ge_than(n, exclude_fd) == 0)
		return 0;

	/* Fallback when close_range fails.  */
	debug("close_range() failed, fallback to close() each open FD: %m");

	dir = opendir("/proc/self/fd");
	if (dir == NULL) {
		debug("cannot opendir /proc/self/fd: %m");
		return -1;
	}

	fd = dirfd(dir);
	for (next = readdir(dir); next; next = readdir(dir)) {
		const char *name = next->d_name;
		long long val;

		if (name[0] == '.')
			continue;

		val = strtoll(name, NULL, 10);
		if (val < n || val == fd || val == exclude_fd)
			continue;

		r = close(val);
		if (r < 0) {
			debug("cannot close %d: %m", val);
			failures++;
		}
	}

	r = closedir(dir);
	if (r < 0) {
		debug("cannot close %d: %m", fd);
		failures++;
	}

	return -failures;
}

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
	int ttyfd = open("/dev/tty", O_RDWR|O_CLOEXEC);
	if (ttyfd < 0) {
		info("using stdin as tty fd: could not open /dev/tty: %m");
		ttyfd = STDIN_FILENO;
	}

	/*
	 * Add TTY signals to ignored mask for pid1. This isn't strictly necessary,
	 * but we do it anyway to avoid pid1 being stopped inadvertently.
	 */
	if (sigaddset(sigmask, SIGTSTP) < 0)
		bail("failed to add SIGTSTP to pid1 mask");
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
	if (ttyfd != STDIN_FILENO)
		close(ttyfd);
	return 0;
}

/*
 * If the LISTEN_PID environment variable is set to the parent pid, rewrite it to
 * point to the current pid.
 */
static void rewrite_listen_pid_env()
{
	char *listen_pid = getenv("LISTEN_PID");
	long long val;

	if (listen_pid == NULL)
		return;

	errno = 0;
	val = strtoll(listen_pid, NULL, 10);
	if (errno == ERANGE) {
		warn("LISTEN_PID has an invalid value");
		return;
	}

	if (val == getppid()) {
		char pid_str[32];
		int r;

		snprintf(pid_str, sizeof(pid_str), "%d", getpid());

		r = setenv("LISTEN_PID", pid_str, 1);
		if (r < 0)
			warn("could not overwrite env variable LISTEN_PID: %m");
	}
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

	rewrite_listen_pid_env();

	/*
	 * We are now in the child. Set up our sigmask, put ourselves in the
	 * foreground, and then finally exec (with the environment inherited).
	 */
	if (make_foreground(sigmask) < 0)
		bail("failed to become foreground: %m");
	if (sigprocmask(SIG_SETMASK, sigmask, NULL) < 0)
		bail("failed to reset sigmask: %m");

	execvpe(file, argv, environ);
	bail("failed to exec pid1: %m");
}

/*
 * Handles any queued zombies which need to be reaped using waitpid(2). We
 * continually wait for child process deaths until none are reported (or we
 * have no children left).
 */
static int reap_zombies(pid_t pid1, int *pid1_exitcode)
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

		/*
		 * There is a special-case for our pid1. If the process exits we
		 * inherit its exit code, otherwise we assume an exit code of 127.
		 * This will cause us to exit immediately, since pid1 is now dead.
		 */
		if (child == pid1) {
			/* Did it die from an exit(2)? */
			if (WIFEXITED(wstatus))
				*pid1_exitcode = WEXITSTATUS(wstatus);
			/* What about from a signal? */
			else if (WIFSIGNALED(wstatus))
				*pid1_exitcode = 128 + WTERMSIG(wstatus);
			/* Is the child actually dead? */
			else if (kill(pid1, 0) < 0)
				*pid1_exitcode = 127;
			/* It hasn't died... */
			else
				warn("received SIGCHLD from pid1 (%d) but it's still alive", pid1);
			continue;
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
	bool kill_pgid = false;
	bool run_as_pause = false;
	const struct option longopts[] = {
		{name: "version", has_arg: no_argument, flag: NULL, val: 'V'},
		{},
	};
	while ((opt = getopt_long(argc, argv, "ghLPV", longopts, NULL)) != -1) {
		switch (opt) {
		case 'g':
			kill_pgid = true;
			break;
		case 'P':
			run_as_pause = true;
			break;
		case 'h':
			help();
			exit(0);
		case 'L':
			license();
			exit(0);
		case 'V':
			version();
			exit(0);
		default:
			usage();
			exit(1);
		}
	}
	argv += optind;
	argc -= optind;
	if (argc < 1 && !run_as_pause)
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
	pid_t pid1 = 0;

	if (!run_as_pause) {
		pid1 = spawn_pid1(argv[0], argv, &pid1_sigmask);
		if (pid1 <= 0)
			bail("failed to spawn pid1: %m");

		/* One final check to make sure that it actually spawned. */
		if (kill(pid1, 0) < 0)
			bail("self-check that pid1 (%d) was spawned failed: %m", pid1);
		debug("pid1 (%d) spawned: %s", pid1, argv[0]);
	}

	if (close_fds_ge_than(3, sfd) < 0)
		warn("failed to close some file descriptor in range >=3");

	/*
	 * The "pid" we send signals to. With -g we send signals to the entire
	 * process group which pid1 is in, which is represented by a -ve pid.
	 */
	pid_t pid1_target = run_as_pause ? 0 : (kill_pgid ? -pid1 : pid1);

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
		case SIGTSTP: case SIGTTOU: case SIGTTIN:
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
			if (reap_zombies(pid1, &pid1_exitcode) < 0)
				warn("problem occurred while reaping zombies: %m");
			break;

		/* A signal sent to us by a user which we must forward to pid1. */
		default:
			/* We just forward the signal to pid1. */
			if (run_as_pause) {
				if (ssi.ssi_signo == SIGTERM || ssi.ssi_signo == SIGINT)
					return 0;
			} else if (kill(pid1_target, ssi.ssi_signo) < 0) {
				warn("forwarding of signal %d to pid1 (%d) failed: %m", ssi.ssi_signo, pid1_target);
			}
			break;
		}
	}
	return pid1_exitcode;
}
