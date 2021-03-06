/*

   nsjail - CLONE_NEWNS routines
   -----------------------------------------

   Copyright 2014 Google Inc. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include "mount.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"
#include "subproc.h"
#include "util.h"

#define VALSTR_STRUCT(x) { x, #x }

#if !defined(MS_LAZYTIME)
#define MS_LAZYTIME (1<<25)
#endif				/* if !defined(MS_LAZYTIME) */

const char *mountFlagsToStr(uintptr_t flags)
{
	static __thread char mountFlagsStr[1024];
	mountFlagsStr[0] = '\0';

	/*  *INDENT-OFF* */
	static struct {
		const uintptr_t flag;
		const char* const name;
	} const mountFlags[] = {
			VALSTR_STRUCT(MS_RDONLY),
			VALSTR_STRUCT(MS_NOSUID),
			VALSTR_STRUCT(MS_NODEV),
			VALSTR_STRUCT(MS_NOEXEC),
			VALSTR_STRUCT(MS_SYNCHRONOUS),
			VALSTR_STRUCT(MS_REMOUNT),
			VALSTR_STRUCT(MS_MANDLOCK),
			VALSTR_STRUCT(MS_DIRSYNC),
			VALSTR_STRUCT(MS_NOATIME),
			VALSTR_STRUCT(MS_NODIRATIME),
			VALSTR_STRUCT(MS_BIND),
			VALSTR_STRUCT(MS_MOVE),
			VALSTR_STRUCT(MS_REC),
			VALSTR_STRUCT(MS_SILENT),
			VALSTR_STRUCT(MS_POSIXACL),
			VALSTR_STRUCT(MS_UNBINDABLE),
			VALSTR_STRUCT(MS_PRIVATE),
			VALSTR_STRUCT(MS_SLAVE),
			VALSTR_STRUCT(MS_SHARED),
			VALSTR_STRUCT(MS_RELATIME),
			VALSTR_STRUCT(MS_KERNMOUNT),
			VALSTR_STRUCT(MS_I_VERSION),
			VALSTR_STRUCT(MS_STRICTATIME),
			VALSTR_STRUCT(MS_LAZYTIME),
	};
	/*  *INDENT-ON* */

	for (size_t i = 0; i < ARRAYSIZE(mountFlags); i++) {
		if (flags & mountFlags[i].flag) {
			utilSSnPrintf(mountFlagsStr, sizeof(mountFlagsStr), "%s|",
				      mountFlags[i].name);
		}
	}

	uintptr_t knownFlagMask = 0U;
	for (size_t i = 0; i < ARRAYSIZE(mountFlags); i++) {
		knownFlagMask |= mountFlags[i].flag;
	}
	utilSSnPrintf(mountFlagsStr, sizeof(mountFlagsStr), "%#tx", flags & ~(knownFlagMask));
	return mountFlagsStr;
}

bool mountIsDir(const char *path)
{
	/*
	 *  If the source dir is NULL, we assume it's a dir (for /proc and tmpfs)
	 */
	if (path == NULL) {
		return true;
	}
	struct stat st;
	if (stat(path, &st) == -1) {
		PLOG_D("stat('%s')", path);
		return false;
	}
	if (S_ISDIR(st.st_mode)) {
		return true;
	}
	return false;
}

static bool mountMount(struct mounts_t *mpt, const char *newroot, const char *tmpdir)
{
	char dst[PATH_MAX];
	snprintf(dst, sizeof(dst), "%s/%s", newroot, mpt->dst);

	LOG_D("Mounting '%s'", mountDescribeMountPt(mpt));

	char srcpath[PATH_MAX];
	if (mpt->src != NULL && strlen(mpt->src) > 0) {
		snprintf(srcpath, sizeof(srcpath), "%s", mpt->src);
	} else {
		snprintf(srcpath, sizeof(srcpath), "none");
	}

	if (mpt->isDir == true) {
		if (utilCreateDirRecursively(dst) == false) {
			LOG_W("Couldn't create upper directories for '%s'", dst);
			return false;
		}
		if (mkdir(dst, 0711) == -1 && errno != EEXIST) {
			PLOG_W("mkdir('%s')", dst);
		}
	} else {
		if (utilCreateDirRecursively(dst) == false) {
			LOG_W("Couldn't create upper directories for '%s'", dst);
			return false;
		}
		int fd = TEMP_FAILURE_RETRY(open(dst, O_CREAT | O_RDONLY | O_CLOEXEC, 0644));
		if (fd >= 0) {
			close(fd);
		} else {
			PLOG_W("open('%s', O_CREAT|O_RDONLY|O_CLOEXEC, 0700)", dst);
		}
	}

	if (mpt->src_content) {
		snprintf(srcpath, sizeof(srcpath), "%s/file.XXXXXX", tmpdir);
		int fd = mkostemp(srcpath, O_CLOEXEC);
		if (fd < 0) {
			PLOG_W("mkostemp('%s')", srcpath);
			close(fd);
			return false;
		}
		if (utilWriteToFd(fd, mpt->src_content, mpt->src_content_len) == false) {
			LOG_W("Writting %zu bytes to '%s' failed", mpt->src_content_len, srcpath);
			close(fd);
			return false;
		}
		close(fd);
		mpt->flags |= (MS_BIND | MS_REC);
	}

	/*
	 * Initially mount it as RW, it will be remounted later on if needed
	 */
	unsigned long flags = mpt->flags & ~(MS_RDONLY);
	if (mount(srcpath, dst, mpt->fs_type, flags, mpt->options) == -1) {
		if (mpt->mandatory == false) {
			PLOG_D("mount('%s') src:'%s' dst:'%s' failed", mountDescribeMountPt(mpt),
			       srcpath, dst);
		} else if (errno == EACCES) {
			PLOG_E("mount('%s') src:'%s' dst:'%s' failed. "
			       "Try fixing this problem by applying 'chmod o+x' to the '%s' directory and "
			       "its ancestors", mountDescribeMountPt(mpt), srcpath, dst, srcpath);
		} else {
			PLOG_E("mount('%s') src:'%s' dst:'%s' failed", mountDescribeMountPt(mpt),
			       srcpath, dst);
		}
		if (mpt->mandatory) {
			return false;
		}
	}

	if (mpt->src_content && unlink(srcpath) == -1) {
		PLOG_W("unlink('%s')", srcpath);
	}
	return true;
}

static bool mountRemountRO(struct mounts_t *mpt)
{
	if (!(mpt->flags & MS_RDONLY)) {
		return true;
	}

	struct statvfs vfs;
	if (TEMP_FAILURE_RETRY(statvfs(mpt->dst, &vfs)) == -1) {
		if (mpt->mandatory) {
			PLOG_E("statvfs('%s')", mpt->dst);
			return false;
		} else {
			PLOG_D("statvfs('%s')", mpt->dst);
			return true;
		}
	}
	/*
	 * It's fine to use 'flags | vfs.f_flag' here as per
	 * /usr/include/x86_64-linux-gnu/bits/statvfs.h: 'Definitions for
	 * the flag in `f_flag'.  These definitions should be
	 * kept in sync with the definitions in <sys/mount.h>'
	 */
	unsigned long new_flags = MS_REMOUNT | MS_RDONLY | vfs.f_flag;

	LOG_D("Re-mounting R/O '%s' (old_flags:%s, new_flags:%s)", mpt->dst,
	      mountFlagsToStr(vfs.f_flag), mountFlagsToStr(new_flags));

	if (mount(mpt->dst, mpt->dst, NULL, new_flags, 0) == -1) {
		if (mpt->mandatory) {
			PLOG_W("mount('%s', flags:%s)", mpt->dst, mountFlagsToStr(new_flags));
			return false;
		}
		PLOG_D("mount('%s', flags:%s)", mpt->dst, mountFlagsToStr(new_flags));
	}

	return true;
}

static bool mountInitNsInternal(struct nsjconf_t *nsjconf)
{
	if (nsjconf->clone_newns == false) {
		if (chroot(nsjconf->chroot) == -1) {
			PLOG_E("chroot('%s')", nsjconf->chroot) {
				return false;
			}
		}
		if (chdir("/") == -1) {
			PLOG_E("chdir('/')");
			return false;
		}
		return true;
	}

	if (chdir("/") == -1) {
		PLOG_E("chdir('/')");
		return false;
	}

	const char *const destdir = "/tmp/nsjail.root";
	if (mkdir(destdir, 0755) == -1 && errno != EEXIST) {
		PLOG_E("Couldn't create '%s' directory. Maybe remove it?", destdir);
		return false;
	}
	if (mount(NULL, destdir, "tmpfs", 0, "size=16777216") == -1) {
		PLOG_E("mount('%s', 'tmpfs')", destdir);
		return false;
	}
	const char *const tmpdir = "/tmp/nsjail.tmp";
	if (mkdir(tmpdir, 0755) == -1 && errno != EEXIST) {
		PLOG_E("Couldn't create '%s' directory. Maybe remove it?", tmpdir);
		return false;
	}
	if (mount(NULL, tmpdir, "tmpfs", 0, "size=16777216") == -1) {
		PLOG_E("mount('%s', 'tmpfs')", tmpdir);
		return false;
	}

	struct mounts_t *p;
	TAILQ_FOREACH(p, &nsjconf->mountpts, pointers) {
		if (mountMount(p, destdir, tmpdir) == false) {
			return false;
		}
	}

	if (umount2(tmpdir, MNT_DETACH) == -1) {
		PLOG_E("umount2('%s', MNT_DETACH)", tmpdir);
		return false;
	}
	if (syscall(__NR_pivot_root, destdir, destdir) == -1) {
		PLOG_E("pivot_root('%s', '%s')", destdir, destdir);
		return false;
	}

	if (umount2("/", MNT_DETACH) == -1) {
		PLOG_E("umount2('/', MNT_DETACH)");
		return false;
	}
	if (chdir(nsjconf->cwd) == -1) {
		PLOG_E("chdir('%s')", nsjconf->cwd);
		return false;
	}

	TAILQ_FOREACH(p, &nsjconf->mountpts, pointers) {
		if (mountRemountRO(p) == false) {
			return false;
		}
	}

	return true;
}

/*
 * With mode MODE_STANDALONE_EXECVE it's required to mount /proc inside a new process,
 * as the current process is still in the original PID namespace (man pid_namespaces)
 */
bool mountInitNs(struct nsjconf_t * nsjconf)
{
	if (nsjconf->mode != MODE_STANDALONE_EXECVE) {
		return mountInitNsInternal(nsjconf);
	}

	pid_t pid = subprocClone(CLONE_FS | SIGCHLD);
	if (pid == -1) {
		return false;
	}

	if (pid == 0) {
		exit(mountInitNsInternal(nsjconf) ? 0 : 1);
	}

	int status;
	while (wait4(pid, &status, 0, NULL) != pid) ;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		return true;
	}
	return false;
}

bool mountAddMountPt(struct nsjconf_t * nsjconf, const char *src, const char *dst,
		     const char *fstype, const char *options, uintptr_t flags, const bool * isDir,
		     bool mandatory, const char *src_env, const char *dst_env,
		     const uint8_t * src_content, size_t src_content_len)
{
	struct mounts_t *p = utilCalloc(sizeof(struct mounts_t));

	if (src_env) {
		const char *e = getenv(src_env);
		if (e == NULL) {
			LOG_W("No such envvar:'%s'", src_env);
			return false;
		}
		if (asprintf((char **)&p->src, "%s%s", e, src ? src : "") == -1) {
			PLOG_W("asprintf() failed");
			return false;
		}
	} else {
		p->src = utilStrDup(src);
	}

	if (dst_env) {
		const char *e = getenv(dst_env);
		if (e == NULL) {
			LOG_W("No such envvar:'%s'", dst_env);
			return false;
		}
		if (asprintf((char **)&p->dst, "%s%s", e, dst ? dst : "") == -1) {
			PLOG_W("asprintf() failed");
			return false;
		}
	} else {
		p->dst = utilStrDup(dst);
	}

	/* Try to canonicalize/realpath the source path if it's mount --bind */
	if (p->src && (flags & MS_BIND)) {
		const char *rp = realpath(p->src, NULL);
		if (rp) {
			free((void *)p->src);
			p->src = rp;
		} else {
			if (mandatory) {
				PLOG_W("realpath('%s') failed", p->src);
			} else {
				PLOG_D("realpath('%s') failed", p->src);
			}
		}
	}

	p->fs_type = utilStrDup(fstype);
	p->options = utilStrDup(options);
	p->flags = flags;
	p->isDir = true;
	p->mandatory = mandatory;

	if (isDir) {
		p->isDir = *isDir;
	} else {
		if (src_content) {
			p->isDir = false;
		} else if (p->src == NULL) {
			p->isDir = true;
		} else if (p->flags & MS_BIND) {
			p->isDir = mountIsDir(p->src);
		} else {
			p->isDir = true;
		}
	}

	p->src_content = utilMemDup(src_content, src_content_len);
	p->src_content_len = src_content_len;

	TAILQ_INSERT_TAIL(&nsjconf->mountpts, p, pointers);

	return true;
}

const char *mountDescribeMountPt(struct mounts_t *mpt)
{
	static __thread char mount_pt_descr[4096];

	snprintf(mount_pt_descr, sizeof(mount_pt_descr),
		 "src:'%s' dst:'%s' type:'%s' flags:%s options:'%s' isDir:%s",
		 mpt->src ? mpt->src : "[NULL]", mpt->dst, mpt->fs_type ? mpt->fs_type : "[NULL]",
		 mountFlagsToStr(mpt->flags), mpt->options ? mpt->options : "[NULL]",
		 mpt->isDir ? "true" : "false");

	if (mpt->mandatory == false) {
		utilSSnPrintf(mount_pt_descr, sizeof(mount_pt_descr), " mandatory:false");
	}
	if (mpt->src_content) {
		utilSSnPrintf(mount_pt_descr, sizeof(mount_pt_descr), " src_content_len:%zu",
			      mpt->src_content_len);
	}

	return mount_pt_descr;
}
