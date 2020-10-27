/*
 * gcc -shared -fPIC -O2 uchroot.c -o uchroot.so -ldl
 * LD_PRELOAD=$PWD/uchroot.so <executeable>
 *
 * OSX
 * gcc -shared -fPIC -o uchroot.dylib uchroot.c
 * DYLD_INSERT_LIBRARIES=$PWD/uchroot.dylib DYLD_FORCE_FLAT_NAMESPACE=1 <executeable>
 *
 * export GSOCKET_DEBUG=1 # enable debug output to stderr
 *
 * Future ideas:
 * - could hijack 'pwd' (realpath) and return "/THC-ROOT/". This means sftp'd
 *   pwd command would not leak $CWD directory name [does anyone care?]
 */
#if 0
List of sftp requests and their corresponding libc calls:

The sftp-server should be limited (-p whitelist) to calls from this list
that are hijacked and check.

N/R == Not Reached. A call that can not be reached without having
gone through a previous call that got checked:
e.g. 'close' is only reached if 'open' was granted and thus checking
'close' is not needed as. Hijacked 'open' would have checked for access
permission already).

SFTP-cmd 		linux		OSX 			SFTP-client		
-------------------------------------------------------
opendir			opendir()	opendir$INODE64	ls /tmp
stat			__xstat()	stat$INODE64()	cd /tmp
lstat			__lxstat()	lstat$INODE64() ls -al /tmp/exists.txt
open			open()		%				get /tmp/exists.txt
mkdir			mkdir()		%				mkdir /tmp/notexists
remove			unlink()	%				rm /tmp/exists.txt
rmdir			rmdir()		%				rmdir /tmp/exists
symlink			symlink()	%				ln -s /etc/hosts etc-hosts-new
hardlink		link()		%				ln /etc/hosts etc-hosts-new
posix-rename	rename()	%				rename /tmp/exists.txt /tmp/0wned.txt
statvfs			statvfs()	%				gs-mount (OSX)
setstat			chmod()		%				chmod 755 /tmp/exists.txt

fsetstat	N/R [open]
fstat		N/R [open]
read		N/R [open]
close		N/R [open]
readdir		N/R [opendir]
realpath	N/R [opendir]
write		N/R [open]
read		N/R [open]
close		N/R [open]

SECURITY: lstat() allows partial match. Many clients use lstat()
to traverse down the directory structure starting from '/'. Returning
EACCES for anything outside the $CWD would break clients who call:

lstat(/home/) -> lstat(/home/user) -> lstat(/home/user/dir1)))

The last call is allowed but the first 2 would not. However, if we
return EACCES to lstat(/home/) then the client would fail even
that the client should not.
#endif	/* END explanation */

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <libgen.h>

static size_t clen;
static char rp_cwd[PATH_MAX + 1];
static int is_init;
static char rp_buf[PATH_MAX + 1];
static int is_debug;
static int is_no_hijack;

#define DEBUGF(a...) do { if (is_debug == 0){break;} fprintf(stderr, "LDP %d:", __LINE__); fprintf(stderr, a); }while(0)
#define D_BRED(a)	"\033[1;31m"a"\033[0m"

static void
thc_init(void)
{
	if (is_init)
		return;

	DEBUGF("%s called\n", __func__);
	if (getenv("GSOCKET_DEBUG"))
		is_debug = 1;

	/* OSX's getcwd() calls stat() */
	char *ptr;
	ptr = getcwd(NULL, 0);
	if (ptr == NULL)
		exit(123);
	if (realpath(ptr, rp_cwd) == NULL)
		exit(124);
	DEBUGF("CWD = %s\n", rp_cwd);

	clen = strlen(rp_cwd);
	is_init = 1;
}

/*
 * 'name' must be the absolute and resolved path.
 * Return 0 if access is allowed
 * 
 * fullmatch is set to 0 for lstat() to allow lstat on directories
 * that are below current CWD.
 */
static int
thc_access(const char *name, const char *fname, int fullmatch)
{
	int len;

	// Access to /dev/null always allowed...
	if (strcmp(name, "/dev/null") == 0)
		return 0;
	// if (strcmp(name, "/AppleInternal") == 0)
		// return 0;
	// if (strcmp(name, "/dev/autofs_nowait") == 0)
		// return 0;
	if (strcmp(name, ".") == 0)
		return 0;

	/* Check if name starts with cwd */
	len = strlen(name);
	if (len >= clen)
	{
		if (memcmp(name, rp_cwd, clen) == 0)
			return 0;
	} else {
		if (fullmatch == 0)
		{
			// HERE: lstat() -> Use partial match.
			// lstat(/home) ALLOWED if CWD==/home/user/downloads
			if ((len >= 1) && memcmp(name, rp_cwd, len) == 0)
				return 0;
		}
	}

	DEBUGF(D_BRED("DENIED")" %s(%s)\n", fname, name);	
	errno = EACCES;
	return -1;
}

/*
 * Return the absolute path of 'path' or the part that does exist.
 */
static char *
thc_realpath(const char *fname, const char *path, char *rp)
{
	char abpath[PATH_MAX + 1];
	const char *ptr;
	char *res;

	ptr = path;
	res = realpath(ptr, rp);
	/* realpath() fails if path does not exist.
	 * Return OK if part of path is within the CWD.
	 * Return EACCESS otherwise.
	 * 
	 * mkdir ./doesnotexist -> ACCESS OK
	 * mkdir /tmp/doesnotexists -> EACCES
	 * From right side try to find file that does exist
	 * and then try to determine thc-access() from the existing
	 * file to find out of EACCES or ENOTFOUND should be returned.
	 * Allowed: /home/alice/x.c
	 * Denied : /home/bob/y.c
	 */
	if (res == NULL)
	{
		/* Local Copy */
		snprintf(abpath, sizeof abpath, "%s", path);
		ptr = abpath;
		char *next;
		while (1)
		{
			res = NULL;
			next = strrchr(ptr, '/');
			if (next == NULL)
				break;
			if (strlen(next) <= 0)
				break;
			*next = '\0';
			if (*ptr == 0)
				ptr = "/";
			/* HERE: either '/' or '/name' */
			DEBUGF("Checking if rp=%s exists [from %s].\n", ptr, path);
			res = realpath(ptr, rp);
			if (res != NULL)
				break;
			if (ptr[1] == 0)
				break;
			/* HERE: Still no existing directory found */
		}

		if (res == NULL)
		{
			DEBUGF("%s-realpath(%s [from %s]) "D_BRED("FAILED")" (errno forced to %s)\n", fname, ptr, path, strerror(errno));
			return "/"; //NULL;
		}
		/* HERE: It exists! Return part of the path that exist */
	}
	DEBUGF("thc-RealPath: %s [was %s->%s] (DONE)\n", rp, path, ptr);

	return rp;
}

/* Check for special file (like /dev/null) & return */
#define ALLOW_FILE_RETURN(xdst, xfile, xmatch)	if (strcmp(xfile, xmatch) == 0) { memcpy(xdst, xmatch, strlen(xmatch) + 1); return xdst; }
/*
 * Return the (real) directory (without file part).
 * Return 0 on success.
 */
static char *
thc_realfile(const char *fname, const char *file, char *dst)
{
	char dirn[PATH_MAX + 1];
	char *ptr;

	DEBUGF("thc_realfile(func=%s, file=%s, dst)\n", fname, file);
	/* Normally return the directory but as a hack and for some special
	 * files we return the file: thc_access is also whitelistening those
	 * files to allow access.
	 */
	ALLOW_FILE_RETURN(dst, file, "/dev/null");
	// ALLOW_FILE_RETURN(dst, file, "/dev/autofs_nowait");

	if (strlen(file) >= sizeof dirn)
		return NULL;
	snprintf(dirn, sizeof dirn, "%s", file);
	ptr = dirname(dirn);

	if (thc_realpath(fname, ptr, dst) == NULL)
		return NULL;

	DEBUGF("Returning '%s'\n", dst);
	return dst;
}


// Construct "int func(int, const char *, void *)"
typedef int (*real_funcintifv_t)(int ver, const char *path, void *buf);
static int real_funcintifv(const char *fname, int ver, const char *path, void *buf) {return ((real_funcintifv_t)dlsym(RTLD_NEXT, fname))(ver, path, buf);}
static int
thc_funcintifv(const char *fname, int ver, const char *path, void *buf, int fullmatch)
{
	DEBUGF("%s(%s)\n", fname, path);
	thc_init();

	if (thc_realpath(fname, path, rp_buf) == NULL)
		return -1;
	if (thc_access(rp_buf, fname, fullmatch) != 0)
		return -1;

	return real_funcintifv(fname, ver, path, buf);
}

int
__xstat(int ver, const char *path, struct stat *buf)
{
	return thc_funcintifv(__func__, ver, path, buf, 1 /* FULL MATCH */);
}

// E.g. we are in /home/user and do 'mkdir dir' then sftp-server will:
// lxstat("/home") -> lxstat("/home/user") -> lxstat("/home/user/dir")
int
__lxstat(int ver, const char *path, struct stat *buf)
{
	return thc_funcintifv(__func__, ver, path, buf, 0 /* ALLOW PARTIAL MATCH */);
}


/*
 * Redirect stub of construct "int func(const char *)"
 */
typedef int (*real_funcintf_t)(const char *file);
static int real_funcintf(const char *fname, const char *file) {return ((real_funcintf_t)dlsym(RTLD_NEXT, fname))(file); }
static int
thc_funcintf(const char *fname, const char *file)
{
	DEBUGF("%s(%s)\n", fname, file);
	thc_init();

	if (thc_realpath(fname, file, rp_buf) == NULL)
		return -1;
	if (thc_access(rp_buf, fname, 1) != 0)
		return -1;

	return real_funcintf(fname, rp_buf);
}

int
unlink(const char *file)
{
	return thc_funcintf(__func__, file);
}

int
rmdir(const char *file)
{
	return thc_funcintf(__func__, file);
}

/*
 * Redirect stub of construct "int func(const char *, const char *)"
 */
typedef int (*real_funcintff_t)(const char *old, const char *new);
static int real_funcintff(const char *fname, const char *old, const char *new) {return ((real_funcintff_t)dlsym(RTLD_NEXT, fname))(old, new); }
static int
thc_funcintff(const char *fname, const char *old, const char *new)
{
	DEBUGF("%s(%s, %s)\n", fname, old, new);
	thc_init();

	if (thc_realpath(fname, old, rp_buf) == NULL)
		return -1;
	if (thc_access(rp_buf, fname, 1) != 0)
		return -1;
	if (thc_realpath(fname, new, rp_buf) == NULL)
		return -1;
	if (thc_access(rp_buf, fname, 1) != 0)
		return -1;

	/* rp_buf holds the directory [not the filename, which may not exist] */
	return real_funcintff(fname, old, new);
}

int
rename(const char *old, const char *new)
{
	return thc_funcintff(__func__, old, new);
}

int
link(const char *path1, const char *path2)
{
	return thc_funcintff(__func__, path1, path2);
}

int
symlink(const char *path1, const char *path2)
{
	return thc_funcintff(__func__, path1, path2);
}

/*
 * Redirect stub of construct "int func(const char *, void *)"
 */
typedef int (*real_funcintfv_t)(const char *file, void *ptr);
static int real_funcintfv(const char *fname, const char *file, void *ptr) {return ((real_funcintff_t)dlsym(RTLD_NEXT, fname))(file, ptr); }
static int
thc_funcintfv(const char *fname, const char *file, void *ptr, int fullmatch)
{
	int err = 0;
	if (is_no_hijack)
		return real_funcintfv(fname, file, ptr);

	is_no_hijack = 1;
	thc_init();

	if (thc_realpath(fname, file, rp_buf) == NULL)
		err = -1;
	else if (thc_access(rp_buf, fname, fullmatch) != 0)
		err = -1;

	if (err == 0)
		err = real_funcintfv(fname, file, ptr);
	is_no_hijack = 0;
	return err;
}

int
statvfs(const char *path, void *buf)
{
	DEBUGF("%s(%s, %p) (no_hijack=%d)\n", __func__, path, buf, is_no_hijack);
	return thc_funcintfv(__func__, path, buf, 1);
}

/*
 * OSX
 */
#ifdef __APPLE__
# define STATFNAME	"stat$INODE64"
# define LSTATFNAME	"lstat$INODE64"
#else /* Solaris. Linux uses __xstat() and __lxstat() */
# define STATFNAME	"stat"
# define LSTATFNAME	"lstat"
#endif

/*
 * OSX & Solaris
 */
int
stat(const char *path, struct stat *buf)
{
	DEBUGF("%s(%s, %p) (no_hijack=%d)\n", __func__, path, buf, is_no_hijack);
		/* allow stat("/"); */
	if (strcmp(path, "/") == 0)
	{
		int ret;
		is_no_hijack = 1;
		ret = real_funcintfv(STATFNAME, path, buf);
		is_no_hijack = 0;
		return ret;
	}

	return thc_funcintfv(STATFNAME, path, buf, 1);
}

int
lstat(const char *path, struct stat *buf)
{
	DEBUGF("%s(%s, %p) (no_hijack=%d)\n", __func__, path, buf, is_no_hijack);

	return thc_funcintfv(LSTATFNAME, path, buf, 0 /* ALLOW PARTIAL MATCH */);	
}

/*
 * Redirect stub of construct "void *func(const char *)"
 */
typedef void *(*real_funcptrf_t)(const char *file);
static void *real_funcptrf(const char *fname, const char *file) {return ((real_funcptrf_t)dlsym(RTLD_NEXT, fname))(file); }
static void *
thc_funcptrf(const char *fname, const char *file)
{
	void *ret_ptr = NULL;
	int err = 0;

	DEBUGF("%s(%s)\n", fname, file);

	if (is_no_hijack)
		return real_funcptrf(fname, rp_buf);

	is_no_hijack = 1;
	thc_init();
	if (thc_realpath(fname, file, rp_buf) == NULL)
		err = -1;
	else if (thc_access(rp_buf, fname, 1) != 0)
		err = -1;

	if (err != 0)
		ret_ptr = NULL;
	else
		ret_ptr = real_funcptrf(fname, rp_buf);

	is_no_hijack = 0;
	return ret_ptr;
}

void *
opendir(const char *file)
{
	return thc_funcptrf(__func__, file);
}

/* OSX */
void *
opendir$INODE64(const char *file)
{
	return thc_funcptrf(__func__, file);
}

/*
 * Redirect stub of construct "int func(const char *, mode_t)"
 */
typedef int (*real_funcintfm_t)(const char *file, mode_t mode);
static int real_funcintfm(const char *fname, const char *file, mode_t mode) {return ((real_funcintfm_t)dlsym(RTLD_NEXT, fname))(file, mode);}
static int
thc_funcintfm(const char *fname, const char *file, mode_t mode)
{
	DEBUGF("%s(%s, %d)\n", fname, file, mode);
	thc_init();

	if (thc_realfile(fname, file, rp_buf) == NULL)
		return -1;

	if (thc_access(rp_buf, fname, 1) != 0)
		return -1;

	return real_funcintfm(fname, file, mode);
}

int
mkdir(const char *path, mode_t mode)
{
	/*
	 * path could be absolute or relative (2x):
	 * "./test"
	 * "test"
	 * "/tmp/test"
	 */
	return thc_funcintfm(__func__, path, mode);
}

int
chmod(const char *file, mode_t mode)
{
	return thc_funcintfm(__func__, file, mode);
}

typedef int (*real_open_t)(const char *file, int flags, mode_t mode);
static int real_open(const char *file, int flags, mode_t mode) {return ((real_open_t)dlsym(RTLD_NEXT, "open"))(file, flags, mode); }
int
open(const char *file, int flags, mode_t mode)
{
	int err = 0;
	DEBUGF("open(%s)\n", file);

	is_no_hijack = 1;
	thc_init();

	if (thc_realfile(__func__, file, rp_buf) == NULL)
		err = -1;
	else if (thc_access(rp_buf, __func__, 1) != 0)
		err = -1;

	if (err == 0)
		err = real_open(file, flags, mode);
	is_no_hijack = 0;
	return err;
}



