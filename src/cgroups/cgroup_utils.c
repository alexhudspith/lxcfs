/* SPDX-License-Identifier: LGPL-2.1+ */

#include "config.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "../macro.h"
#include "../memory_utils.h"
#include "cgroup.h"
#include "cgroup_utils.h"
#include "../utils.h"

int get_cgroup_version(char *line)
{
	if (is_cgroupfs_v1(line))
		return CGROUP_SUPER_MAGIC;

	if (is_cgroupfs_v2(line))
		return CGROUP2_SUPER_MAGIC;

	return 0;
}

bool is_cgroupfs_v1(char *line)
{
	char *p = strstr(line, " - ");
	if (!p)
		return false;
	return strncmp(p, " - cgroup ", 10) == 0;
}

bool is_cgroupfs_v2(char *line)
{
	char *p = strstr(line, " - ");
	if (!p)
		return false;

	return strncmp(p, " - cgroup2 ", 11) == 0;
}

int unified_cgroup_hierarchy(void)
{

	int ret;
	struct statfs fs;

	ret = statfs(DEFAULT_CGROUP_MOUNTPOINT, &fs);
	if (ret < 0)
		return -ENOMEDIUM;

	if (is_fs_type(&fs, CGROUP2_SUPER_MAGIC))
		return CGROUP2_SUPER_MAGIC;

	return 0;
}

bool is_cgroup_fd(int fd)
{

	int ret;
	struct statfs fs;

	ret = fstatfs(fd, &fs);
	if (ret)
		return false;

	if (is_fs_type(&fs, CGROUP2_SUPER_MAGIC) ||
	    is_fs_type(&fs, CGROUP_SUPER_MAGIC))
		return true;

	return false;
}

void *must_realloc(void *orig, size_t sz)
{
	void *ret;

	do {
		ret = realloc(orig, sz);
	} while (!ret);

	return ret;
}

char *must_make_path(const char *first, ...)
{
	va_list args;
	char *cur, *dest;
	size_t full_len = strlen(first);
	size_t buf_len;
	size_t cur_len;

	dest = must_copy_string(first);
	cur_len = full_len;

	va_start(args, first);
	while ((cur = va_arg(args, char *)) != NULL) {
		buf_len = strlen(cur);

		full_len += buf_len;
		if (cur[0] != '/')
			full_len++;

		dest = must_realloc(dest, full_len + 1);

		if (cur[0] != '/') {
			memcpy(dest + cur_len, "/", 1);
			cur_len++;
		}

		memcpy(dest + cur_len, cur, buf_len);
		cur_len += buf_len;
	}
	va_end(args);

	dest[cur_len] = '\0';
	return dest;
}

bool is_fs_type(const struct statfs *fs, fs_type_magic magic_val)
{
	return (fs->f_type == (fs_type_magic)magic_val);
}

char *must_copy_string(const char *entry)
{
	char *ret;

	if (!entry)
		return NULL;

	do {
		ret = strdup(entry);
	} while (!ret);

	return ret;
}

char *lxc_string_join(const char *sep, const char **parts, bool use_as_prefix)
{
	char *result;
	char **p;
	size_t sep_len = strlen(sep);
	size_t result_len = use_as_prefix * sep_len;
	size_t buf_len;

	/* calculate new string length */
	for (p = (char **)parts; *p; p++)
		result_len += (p > (char **)parts) * sep_len + strlen(*p);

	buf_len = result_len + 1;
	result = calloc(buf_len, 1);
	if (!result)
		return NULL;

	if (use_as_prefix)
		(void)strlcpy(result, sep, buf_len);

	for (p = (char **)parts; *p; p++) {
		if (p > (char **)parts)
			(void)strlcat(result, sep, buf_len);

		(void)strlcat(result, *p, buf_len);
	}

	return result;
}

int lxc_count_file_lines(const char *fn)
{
	__do_fclose FILE *f = NULL;
	__do_free char *line = NULL;
	size_t sz = 0;
	int n = 0;

	f = fopen_cloexec(fn, "r");
	if (!f)
		return -1;

	while (getline(&line, &sz, f) != -1)
		n++;

	return n;
}

bool dir_exists(const char *path)
{
	struct stat sb;
	int ret;

	ret = stat(path, &sb);
	if (ret < 0)
		/* Could be something other than eexist, just say "no". */
		return false;

	return S_ISDIR(sb.st_mode);
}

/*
 * @path:    a pathname where / replaced with '\0'.
 * @offsetp: pointer to int showing which path segment was last seen.
 *           Updated on return to reflect the next segment.
 * @fulllen: full original path length.
 * Returns a pointer to the next path segment, or NULL if done.
 */
static char *get_nextpath(char *path, int *offsetp, int fulllen)
{
	int offset = *offsetp;

	if (offset >= fulllen)
		return NULL;

	while (offset < fulllen && path[offset] != '\0')
		offset++;

	while (offset < fulllen && path[offset] == '\0')
		offset++;

	*offsetp = offset;

	return (offset < fulllen) ? &path[offset] : NULL;
}

/*
 * Check that @subdir is a subdir of @dir.  @len is the length of
 * @dir (to avoid having to recalculate it).
 */
static bool is_subdir(const char *subdir, const char *dir, size_t len)
{
	size_t subdirlen = strlen(subdir);

	if (subdirlen < len)
		return false;

	if (strncmp(subdir, dir, len) != 0)
		return false;

	if (dir[len-1] == '/')
		return true;

	if (subdir[len] == '/' || subdirlen == len)
		return true;

	return false;
}

/*
 * Check if the open fd is a symlink.  Return -ELOOP if it is.  Return
 * -ENOENT if we couldn't fstat.  Return 0 if the fd is ok.
 */
static int check_symlink(int fd)
{
	struct stat sb;
	int ret;

	ret = fstat(fd, &sb);
	if (ret < 0)
		return -ENOENT;

	if (S_ISLNK(sb.st_mode))
		return -ELOOP;

	return 0;
}

/*
 * Open a file or directory, provided that it contains no symlinks.
 *
 * CAVEAT: This function must not be used for other purposes than container
 * setup before executing the container's init
 */
static int open_if_safe(int dirfd, const char *nextpath)
{
	__do_close int newfd = -EBADF;

	newfd = openat(dirfd, nextpath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (newfd >= 0) /* Was not a symlink, all good. */
		return move_fd(newfd);

	if (errno == ELOOP)
		return -1;

	if (errno == EPERM || errno == EACCES) {
		/* We're not root (cause we got EPERM) so try opening with
		 * O_PATH.
		 */
		newfd = openat(dirfd, nextpath, O_PATH | O_NOFOLLOW);
		if (newfd >= 0) {
			/* O_PATH will return an fd for symlinks. We know
			 * nextpath wasn't a symlink at last openat, so if fd is
			 * now a link, then something * fishy is going on.
			 */
			int ret = check_symlink(newfd);
			if (ret < 0)
				return -1;
		}
	}

	return move_fd(newfd);
}

/*
 * Open a path intending for mounting, ensuring that the final path
 * is inside the container's rootfs.
 *
 * CAVEAT: This function must not be used for other purposes than container
 * setup before executing the container's init
 *
 * @target: path to be opened
 * @prefix_skip: a part of @target in which to ignore symbolic links.  This
 * would be the container's rootfs.
 *
 * Return an open fd for the path, or <0 on error.
 */
static int open_without_symlink(const char *target, const char *prefix_skip)
{
	__do_close int dirfd = -EBADF;
	__do_free char *dup = NULL;
	int curlen = 0, fulllen, i;

	fulllen = strlen(target);

	/* make sure prefix-skip makes sense */
	if (prefix_skip && strlen(prefix_skip) > 0) {
		curlen = strlen(prefix_skip);
		if (!is_subdir(target, prefix_skip, curlen))
			return -EINVAL;

		/*
		 * get_nextpath() expects the curlen argument to be
		 * on a  (turned into \0) / or before it, so decrement
		 * curlen to make sure that happens
		 */
		if (curlen)
			curlen--;
	} else {
		prefix_skip = "/";
		curlen = 0;
	}

	/* Make a copy of target which we can hack up, and tokenize it */
	dup = strdup(target);
	if (!dup)
		return ret_errno(ENOMEM);

	for (i = 0; i < fulllen; i++) {
		if (dup[i] == '/')
			dup[i] = '\0';
	}

	dirfd = open(prefix_skip, O_RDONLY);
	if (dirfd < 0)
		return -1;

	for (;;) {
		int newfd;
		char *nextpath;

		nextpath = get_nextpath(dup, &curlen, fulllen);
		if (!nextpath)
			return move_fd(dirfd);

		newfd = open_if_safe(dirfd, nextpath);
		close_prot_errno_disarm(dirfd);
		dirfd = newfd;
		if (newfd < 0)
			return -1;
	}

	return move_fd(dirfd);
}

/*
 * Safely mount a path into a container, ensuring that the mount target
 * is under the container's @rootfs.  (If @rootfs is NULL, then the container
 * uses the host's /)
 *
 * CAVEAT: This function must not be used for other purposes than container
 * setup before executing the container's init
 */
int safe_mount(const char *src, const char *dest, const char *fstype,
	       unsigned long flags, const void *data, const char *rootfs)
{
	__do_close int destfd = -EBADF, srcfd = -EBADF;
	int ret;
	/* Only needs enough for /proc/self/fd/<fd>. */
	char srcbuf[50], destbuf[50];
	const char *mntsrc = src;

	if (!rootfs)
		rootfs = "";

	/* todo - allow symlinks for relative paths if 'allowsymlinks' option is passed */
	if (flags & MS_BIND && src && src[0] != '/') {

		srcfd = open_without_symlink(src, NULL);
		if (srcfd < 0)
			return srcfd;

		ret = snprintf(srcbuf, sizeof(srcbuf), "/proc/self/fd/%d", srcfd);
		if (ret < 0 || ret >= (int)sizeof(srcbuf))
			return -EINVAL;
		mntsrc = srcbuf;
	}

	destfd = open_without_symlink(dest, rootfs);
	if (destfd < 0)
		return -1;

	ret = snprintf(destbuf, sizeof(destbuf), "/proc/self/fd/%d", destfd);
	if (ret < 0 || ret >= (int)sizeof(destbuf))
		return ret_errno(EINVAL);

	ret = mount(mntsrc, destbuf, fstype, flags, data);
	if (ret < 0)
		return -1;

	return 0;
}

#if !HAVE_STRLCPY
size_t strlcpy(char *dest, const char *src, size_t size)
{
	size_t ret = strlen(src);

	if (size) {
		size_t len = (ret >= size) ? size - 1 : ret;
		memcpy(dest, src, len);
		dest[len] = '\0';
	}

	return ret;
}
#endif

#if !HAVE_STRLCAT
size_t strlcat(char *d, const char *s, size_t n)
{
	size_t l = strnlen(d, n);
	if (l == n)
		return l + strlen(s);

	return l + strlcpy(d + l, s, n - l);
}
#endif

FILE *fopen_cloexec(const char *path, const char *mode)
{
	__do_close int fd = -EBADF;
	__do_fclose FILE *ret = NULL;
	int open_mode = 0;
	int step = 0;

	if (!strncmp(mode, "r+", 2)) {
		open_mode = O_RDWR;
		step = 2;
	} else if (!strncmp(mode, "r", 1)) {
		open_mode = O_RDONLY;
		step = 1;
	} else if (!strncmp(mode, "w+", 2)) {
		open_mode = O_RDWR | O_TRUNC | O_CREAT;
		step = 2;
	} else if (!strncmp(mode, "w", 1)) {
		open_mode = O_WRONLY | O_TRUNC | O_CREAT;
		step = 1;
	} else if (!strncmp(mode, "a+", 2)) {
		open_mode = O_RDWR | O_CREAT | O_APPEND;
		step = 2;
	} else if (!strncmp(mode, "a", 1)) {
		open_mode = O_WRONLY | O_CREAT | O_APPEND;
		step = 1;
	}
	for (; mode[step]; step++)
		if (mode[step] == 'x')
			open_mode |= O_EXCL;
	open_mode |= O_CLOEXEC;

	fd = open(path, open_mode, 0660);
	if (fd < 0)
		return NULL;

	ret = fdopen(fd, mode);
	if (!ret)
		return NULL;
	move_fd(fd);

	return move_ptr(ret);
}

/* Given a multi-line string, return a null-terminated copy of the current line. */
static char *copy_to_eol(char *p)
{
	char *p2 = strchr(p, '\n'), *sret;
	size_t len;

	if (!p2)
		return NULL;

	len = p2 - p;
	sret = must_realloc(NULL, len + 1);
	memcpy(sret, p, len);
	sret[len] = '\0';
	return sret;
}

static void batch_realloc(char **mem, size_t oldlen, size_t newlen)
{
	int newbatches = (newlen / BATCH_SIZE) + 1;
	int oldbatches = (oldlen / BATCH_SIZE) + 1;

	if (!*mem || newbatches > oldbatches) {
		*mem = must_realloc(*mem, newbatches * BATCH_SIZE);
	}
}

void append_line(char **dest, size_t oldlen, char *new, size_t newlen)
{
	size_t full = oldlen + newlen;

	batch_realloc(dest, oldlen, full + 1);

	memcpy(*dest + oldlen, new, newlen + 1);
}

static inline void drop_trailing_newlines(char *s)
{
	int l;

	for (l = strlen(s); l > 0 && s[l - 1] == '\n'; l--)
		s[l - 1] = '\0';
}

/* Slurp in a whole file */
char *read_file(const char *fnam)
{
	__do_free char *line = NULL;
	__do_fclose FILE *f = NULL;
	int linelen;
	char *buf = NULL;
	size_t len = 0, fulllen = 0;

	f = fopen(fnam, "re");
	if (!f)
		return NULL;
	while ((linelen = getline(&line, &len, f)) != -1) {
		append_line(&buf, fulllen, line, linelen);
		fulllen += linelen;
	}
	return buf;
}

char *read_file_strip_newline(const char *fnam)
{
	char *buf;

	buf = read_file(fnam);
	if (buf)
		drop_trailing_newlines(buf);
	return buf;
}

/* Get current cgroup from /proc/self/cgroup for the cgroupfs v2 hierarchy. */
char *cg_unified_get_current_cgroup(pid_t pid)
{
	__do_free char *basecginfo = NULL;
	char path[STRLITERALLEN("/proc//cgroup") + INTTYPE_TO_STRLEN(pid_t) + 1];
	char *base_cgroup;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid > 0 ? pid : 1);
	basecginfo = read_file(path);
	if (!basecginfo)
		return NULL;

	base_cgroup = strstr(basecginfo, "0::/");
	if (!base_cgroup)
		return NULL;

	base_cgroup = base_cgroup + 3;
	return copy_to_eol(base_cgroup);
}

/* cgline: pointer to character after the first ':' in a line in a \n-terminated
 * /proc/self/cgroup file. Check whether controller c is present.
 */
static bool controller_in_clist(char *cgline, const char *c)
{
	__do_free char *tmp = NULL;
	char *tok, *eol;
	size_t len;

	eol = strchr(cgline, ':');
	if (!eol)
		return false;

	len = eol - cgline;
	tmp = must_realloc(NULL, len + 1);
	memcpy(tmp, cgline, len);
	tmp[len] = '\0';

	lxc_iterate_parts(tok, tmp, ",")
		if (strcmp(tok, c) == 0)
			return true;

	return false;
}

/* @basecginfo is a copy of /proc/$$/cgroup. Return the current cgroup for
 * @controller.
 */
char *cg_hybrid_get_current_cgroup(char *basecginfo, const char *controller, int type)
{
	char *p = basecginfo;

	for (;;) {
		bool is_cgv2_base_cgroup = false;

		/* cgroup v2 entry in "/proc/<pid>/cgroup": "0::/some/path" */
		if ((type == CGROUP2_SUPER_MAGIC) && (*p == '0'))
			is_cgv2_base_cgroup = true;

		p = strchr(p, ':');
		if (!p)
			return NULL;
		p++;

		if (is_cgv2_base_cgroup || (controller && controller_in_clist(p, controller))) {
			p = strchr(p, ':');
			if (!p)
				return NULL;
			p++;
			return copy_to_eol(p);
		}

		p = strchr(p, '\n');
		if (!p)
			return NULL;
		p++;
	}
}

char *cg_legacy_get_current_cgroup(pid_t pid, const char *controller)
{
	__do_free char *basecginfo = NULL;
	char path[STRLITERALLEN("/proc//cgroup") + INTTYPE_TO_STRLEN(pid_t) + 1];

	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid > 0 ? pid : 1);
	basecginfo = read_file(path);
	if (!basecginfo)
		return ret_set_errno(NULL, ENOMEM);

	return cg_hybrid_get_current_cgroup(basecginfo, controller,
					    CGROUP_SUPER_MAGIC);
}


char *readat_file(int dirfd, const char *path)
{
	__do_close int fd = -EBADF;
	__do_free char *line = NULL;
	__do_fclose FILE *f = NULL;
	char *buf = NULL;
	size_t len = 0, fulllen = 0;
	ssize_t linelen;

	fd = openat(dirfd, path, O_NOFOLLOW | O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	f = fdopen(fd, "re");
	if (!f)
		return NULL;
	/* Transfer ownership of fd */
	move_fd(fd);

	while ((linelen = getline(&line, &len, f)) != -1) {
		append_line(&buf, fulllen, line, linelen);
		fulllen += linelen;
	}

	if (buf)
		drop_trailing_newlines(buf);

	return buf;
}

bool mkdir_p(const char *dir, mode_t mode)
{
	const char *tmp = dir;
	const char *orig = dir;
	char *makeme;

	do {
		dir = tmp + strspn(tmp, "/");
		tmp = dir + strcspn(dir, "/");
		makeme = strndup(orig, dir - orig);
		if (!makeme)
			return false;
		if (mkdir(makeme, mode) && errno != EEXIST) {
			lxcfs_error("Failed to create directory '%s': %s.\n",
				makeme, strerror(errno));
			free(makeme);
			return false;
		}
		free(makeme);
	} while(tmp != dir);

	return true;
}

/*
 * This function is taken from glibc-2.32, as POSIX dirname("/some-dir") will
 * return "/some-dir" as opposed to "/", which breaks `get_min_memlimit()`
 */
static char *gnu_dirname(char *path)
{
	static const char dot[] = ".";
	char *last_slash;

	/* Find last '/'.  */
	last_slash = path != NULL ? strrchr(path, '/') : NULL;

	if (last_slash != NULL && last_slash != path && last_slash[1] == '\0') {
		/* Determine whether all remaining characters are slashes.  */
		char *runp;

		for (runp = last_slash; runp != path; --runp)
			if (runp[-1] != '/')
				break;

		/* The '/' is the last character, we have to look further.  */
		if (runp != path)
			last_slash = memrchr(path, '/', runp - path);
	}

	if (last_slash != NULL) {
		/* Determine whether all remaining characters are slashes.  */
		char *runp;

		for (runp = last_slash; runp != path; --runp)
			if (runp[-1] != '/')
				break;

		/* Terminate the path.  */
		if (runp == path) {
			/*
			 * The last slash is the first character in the string.
			 * We have to return "/".  As a special case we have to
			 * return "//" if there are exactly two slashes at the
			 * beginning of the string.  See XBD 4.10 Path Name
			 * Resolution for more information
			 */
			if (last_slash == path + 1)
				++last_slash;
			else
				last_slash = path + 1;
		} else
			last_slash = runp;

		last_slash[0] = '\0';
	} else {
		/*
		 * This assignment is ill-designed but the XPG specs require to
		 * return a string containing "." in any case no directory part
		 * is found and so a static and constant string is required.
		 */
		path = (char *)dot;
	}

	return path;
}

static bool same_file(int fd1, int fd2)
{
	struct stat st1, st2;

	if (fstat(fd1, &st1) < 0 || fstat(fd2, &st2) < 0)
		return false;

	return (st1.st_dev == st2.st_dev) && (st1.st_ino == st2.st_ino);
}

/**
 * Updates metrics with the usage and limit read from a single cgroup directory.
 * Since a controller can be disabled at any level via cgroup.subtree_control,
 * it is allowed for either file to be missing. It is an error if a file exists
 * but can't be parsed.
 *
 * @param dir_fd file descriptor for the cgroup directory
 * @param usage_file_name usage/current file in cgroup
 * @param limit_file_name limit/max file in cgroup
 * @param metrics (output) metrics to update
 * @param is_starting_cgroup `true` to also update the 'usage' field
 * @return 0 on success (and updates `*metrics`); `-errno` if an error occurred
 */
static int update(int dir_fd,
		  const char *usage_file_name, const char *limit_file_name,
		  struct metrics *metrics, bool is_starting_cgroup)
{
	__do_free char *val = NULL;
	uint64_t tmp_usage = 0, tmp_limit = UINT64_MAX;

	val = readat_file(dir_fd, usage_file_name);
	if (val != NULL && safe_uint64(val, &tmp_usage, 10) < 0) {
		return log_error(-errno, "(%d): Can't parse '%s' in %s",
				 errno, val, usage_file_name);
	}

	free_disarm(val);

	val = readat_file(dir_fd, limit_file_name);
	if (val != NULL && strcmp(val, "max") != 0 && safe_uint64(val, &tmp_limit, 10) < 0) {
		return log_error(-errno, "(%d): Can't parse '%s' in %s",
				 errno, val, limit_file_name);
	}

	if (is_starting_cgroup)
		metrics->usage = tmp_usage;

	if (tmp_limit < metrics->effective_limit) {
		// More restrictive limit found, record its usage
		metrics->effective_limit = tmp_limit;
		metrics->usage_at_limit_cgroup = tmp_usage;
	}

	if (tmp_usage > metrics->usage_at_limit_cgroup)
		metrics->usage_at_limit_cgroup = tmp_usage;

	return 0;
}

/**
 * Walk upwards to cgroup root, calculating the usage and effective
 * (hierarchical) limit. The limit will be UINT64_MAX no limit applies
 * in the hierarchy.
 *
 * @param hierarchy_fd file descriptor for the hierarchy
 * @param cgroup_rel starting cgroup directory relative to hierarchy_fd
 * @param usage_file_name usage/current file in cgroup
 * @param limit_file_name limit/max file in cgroup
 * @param metrics (output) metrics to set
 * @returns 0 on success; `-errno` if an error occurred
 */
int cgroup_walkup_to_root(int hierarchy_fd, const char *cgroup_rel,
			  const char *usage_file_name, const char *limit_file_name,
			  struct metrics *metrics)
{
	__do_close int dir_fd = -EBADF;
	int res;
	struct metrics tmp_metrics = new_metrics();

#ifdef DEBUG
	__do_free char *path = strdup(cgroup_rel);
	if (path != NULL) {
		lxcfs_debug("%s", path);
	}
#endif
	/* Open the lowest level, starting cgroup directory */
	dir_fd = openat(hierarchy_fd, cgroup_rel, O_DIRECTORY | O_PATH | O_CLOEXEC);
	if (dir_fd < 0)
		return -errno;

	res = update(dir_fd, usage_file_name, limit_file_name, &tmp_metrics, true);
	if (res < 0)
		return res;

	if (same_file(hierarchy_fd, dir_fd)) {
		*metrics = tmp_metrics;
		return 0;
	}

	/*
	 * Set an arbitrarily hard-coded limit to prevent us from ending
	 * up in an endless loop. There really shouldn't be any cgroup
	 * tree that is 1000 levels deep. That would be insane in
	 * principal and performance-wise.
	 */
	for (int i = 0; i < 1000; i++) {
		__do_close int inner_fd = -EBADF;
#ifdef DEBUG
		if (path != NULL) {
			path = gnu_dirname(path);
			lxcfs_debug("%s", path);
		}
#endif
		inner_fd = move_fd(dir_fd);
		dir_fd = openat(inner_fd, "..", O_DIRECTORY | O_PATH | O_CLOEXEC);
		if (dir_fd < 0)
			return -errno;

		/*
		 * We're at the root of the cgroup tree so stop walking
		 * upwards.
		 * Note that we're not checking the root cgroup itself simply
		 * because a lot of the controllers don't expose files with
		 * limits to the root cgroup.
		 */
		if (same_file(hierarchy_fd, dir_fd)) {
			*metrics = tmp_metrics;
			return 0;
		}

		res = update(dir_fd, usage_file_name, limit_file_name, &tmp_metrics, false);
		if (res < 0)
			return res;
	}

	return log_error_errno(-ELOOP, ELOOP, "To many nested cgroups or invalid mount tree. Terminating walk");
}
