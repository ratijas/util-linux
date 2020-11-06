/*
 * This code is in the public domain; do with it what you wish.
 *
 * Copyright (C) 2012 Sami Kerola <kerolasa@iki.fi>
 * Copyright (C) 2012-2020 Karel Zak <kzak@redhat.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#ifdef HAVE_SENDFILE
#include <sys/sendfile.h>
#endif
#include <string.h>

#include "c.h"
#include "fileutils.h"
#include "pathnames.h"

int mkstemp_cloexec(char *template)
{
#ifdef HAVE_MKOSTEMP
	return mkostemp(template, O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC);
#else
	int fd, old_flags, errno_save;

	fd = mkstemp(template);
	if (fd < 0)
		return fd;

	old_flags = fcntl(fd, F_GETFD, 0);
	if (old_flags < 0)
		goto unwind;
	if (fcntl(fd, F_SETFD, old_flags | O_CLOEXEC) < 0)
		goto unwind;

	return fd;

unwind:
	errno_save = errno;
	unlink(template);
	close(fd);
	errno = errno_save;

	return -1;
#endif
}

/* Create open temporary file in safe way.  Please notice that the
 * file permissions are -rw------- by default. */
int xmkstemp(char **tmpname, const char *dir, const char *prefix)
{
	char *localtmp;
	const char *tmpenv;
	mode_t old_mode;
	int fd, rc;

	/* Some use cases must be capable of being moved atomically
	 * with rename(2), which is the reason why dir is here.  */
	tmpenv = dir ? dir : getenv("TMPDIR");
	if (!tmpenv)
		tmpenv = _PATH_TMP;

	rc = asprintf(&localtmp, "%s/%s.XXXXXX", tmpenv, prefix);
	if (rc < 0)
		return -1;

	old_mode = umask(077);
	fd = mkstemp_cloexec(localtmp);
	umask(old_mode);
	if (fd == -1) {
		free(localtmp);
		localtmp = NULL;
	}
	*tmpname = localtmp;
	return fd;
}

int dup_fd_cloexec(int oldfd, int lowfd)
{
	int fd, flags, errno_save;

#ifdef F_DUPFD_CLOEXEC
	fd = fcntl(oldfd, F_DUPFD_CLOEXEC, lowfd);
	if (fd >= 0)
		return fd;
#endif

	fd = dup(oldfd);
	if (fd < 0)
		return fd;

	flags = fcntl(fd, F_GETFD);
	if (flags < 0)
		goto unwind;
	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		goto unwind;

	return fd;

unwind:
	errno_save = errno;
	close(fd);
	errno = errno_save;

	return -1;
}

/*
 * portable getdtablesize()
 */
int get_fd_tabsize(void)
{
	int m;

#if defined(HAVE_GETDTABLESIZE)
	m = getdtablesize();
#elif defined(HAVE_GETRLIMIT) && defined(RLIMIT_NOFILE)
	struct rlimit rl;

	getrlimit(RLIMIT_NOFILE, &rl);
	m = rl.rlim_cur;
#elif defined(HAVE_SYSCONF) && defined(_SC_OPEN_MAX)
	m = sysconf(_SC_OPEN_MAX);
#else
	m = OPEN_MAX;
#endif
	return m;
}

static inline int in_set(int x, const int set[], size_t setsz)
{
	size_t i;

	for (i = 0; i < setsz; i++) {
		if (set[i] == x)
			return 1;
	}
	return 0;
}

void close_all_fds(const int exclude[], size_t exsz)
{
	struct dirent *d;
	DIR *dir;

	dir = opendir(_PATH_PROC_FDDIR);
	if (dir) {
		while ((d = xreaddir(dir))) {
			char *end;
			int fd;

			errno = 0;
			fd = strtol(d->d_name, &end, 10);

			if (errno || end == d->d_name || !end || *end)
				continue;
			if (dirfd(dir) == fd)
				continue;
			if (in_set(fd, exclude, exsz))
				continue;
			close(fd);
		}
		closedir(dir);
	} else {
		int fd, tbsz = get_fd_tabsize();

		for (fd = 0; fd < tbsz; fd++) {
			if (!in_set(fd, exclude, exsz))
				close(fd);
		}
	}
}

#ifdef TEST_PROGRAM_FILEUTILS
int main(int argc, char *argv[])
{
	if (argc < 2)
		errx(EXIT_FAILURE, "Usage %s --{mkstemp,close-fds}", argv[0]);

	if (strcmp(argv[1], "--mkstemp") == 0) {
		FILE *f;
		char *tmpname;
		f = xfmkstemp(&tmpname, NULL, "test");
		unlink(tmpname);
		free(tmpname);
		fclose(f);

	} else if (strcmp(argv[1], "--close-fds") == 0) {
		static const int wanted_fds[] = {
			STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO
		};

		ignore_result( dup(STDIN_FILENO) );
		ignore_result( dup(STDIN_FILENO) );
		ignore_result( dup(STDIN_FILENO) );

		close_all_fds(wanted_fds, ARRAY_SIZE(wanted_fds));
	}
	return EXIT_SUCCESS;
}
#endif


int mkdir_p(const char *path, mode_t mode)
{
	char *p, *dir;
	int rc = 0;

	if (!path || !*path)
		return -EINVAL;

	dir = p = strdup(path);
	if (!dir)
		return -ENOMEM;

	if (*p == '/')
		p++;

	while (p && *p) {
		char *e = strchr(p, '/');
		if (e)
			*e = '\0';
		if (*p) {
			rc = mkdir(dir, mode);
			if (rc && errno != EEXIST)
				break;
			rc = 0;
		}
		if (!e)
			break;
		*e = '/';
		p = e + 1;
	}

	free(dir);
	return rc;
}

/* returns basename and keeps dirname in the @path, if @path is "/" (root)
 * then returns empty string */
char *stripoff_last_component(char *path)
{
	char *p = path ? strrchr(path, '/') : NULL;

	if (!p)
		return NULL;
	*p = '\0';
	return p + 1;
}

static int copy_file_simple(int from, int to)
{
	ssize_t nr, nw, off;
	char buf[8 * 1024];

	while ((nr = read(from, buf, sizeof(buf))) > 0)
		for (off = 0; nr > 0; nr -= nw, off += nw)
			if ((nw = write(to, buf + off, nr)) < 0)
				return -2;
	if (nr < 0)
		return -1;
#ifdef HAVE_EXPLICIT_BZERO
	explicit_bzero(buf, sizeof(buf));
#endif
	return 0;
}

/* Copies the contents of a file. Returns -1 on read error, -2 on write error. */
int ul_copy_file(int from, int to)
{
#ifdef HAVE_SENDFILE
	struct stat st;
	ssize_t nw;
	off_t left;

	if (fstat(from, &st) == -1)
		return -1;
	if (!S_ISREG(st.st_mode))
		return copy_file_simple(from, to);
	for (left = st.st_size; left != 0; left -= nw) {
		if ((nw = sendfile(to, from, NULL, left)) < 0)
			return copy_file_simple(from, to);
		if (!nw)
			return 0;
	}
	/* For extra robustness, treat st_size as advisory and ensure that we
	 * actually get EOF. */
	while ((nw = sendfile(to, from, NULL, 1024*1024)) != 0)
		if (nw < 0)
			return copy_file_simple(from, to);
	return 0;
#else
	return copy_file_simple(from, to);
#endif
}
