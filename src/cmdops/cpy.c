#include <sys/dents.h>
#include <sys/file.h>
#include <sys/fpath.h>
#include <sys/fprop.h>
#include <sys/mman.h>
#include <sys/splice.h>

#include <errtag.h>
#include <format.h>
#include <string.h>
#include <util.h>

ERRTAG("cpy");

#define RWBUFSIZE 1024*1024

struct top {
	int argc;
	char** argv;
	int argi;
	int opts;

	char* buf;
	long len;
};

struct atfd {
	int at;
	char* dir;
	char* name;
	int fd;
};

struct atdir {
	int at;
	char* dir;
};

struct cct {
	struct top* top;
	struct atdir dst;
	struct atdir src;
	int nosf;
};

#define CTX struct top* ctx
#define SRC struct atfd* src
#define DST struct atfd* dst
#define CCT struct cct* cct

#define OPTS "ntho"
#define OPT_n (1<<0)
#define OPT_t (1<<1)
#define OPT_h (1<<2)
#define OPT_o (1<<3)

#define noreturn __attribute__((noreturn))

static void copy(CCT, char* dstname, char* srcname, int type);
static void failat(const char* msg, char* dir, char* name, int err) noreturn;

/* Utils for dealing with at-dirs. This tool always uses (at, name) pairs
   to access files, but needs full paths when reporting errors and such. */

static int pathlen(char* dir, char* name)
{
	int len = strlen(name);

	if(dir && dir[0] != '/')
		len += strlen(dir) + 1;

	return len + 1;
}

static void makepath(char* buf, int size, char* dir, char* name)
{
	char* p = buf;
	char* e = buf + size - 1;

	if(dir && dir[0] != '/') {
		p = fmtstr(p, e, dir);
		p = fmtstr(p, e, "/");
	}

	p = fmtstr(p, e, name);
	*p = '\0';
}

static void warnat(const char* msg, char* dir, char* name, int err)
{
	int plen = pathlen(dir, name);
	char path[plen];

	makepath(path, sizeof(path), dir, name);

	warn(msg, path, err);
}

static void failat(const char* msg, char* dir, char* name, int err)
{
	warnat(msg, dir, name, err);
	_exit(-1);
}

/* Contents trasfer for regular files */

static int sendfile(CCT, DST, SRC, uint64_t* size)
{
	uint64_t done = 0;
	uint64_t need = *size;
	long ret = 0;
	long run = 0x7ffff000;

	int outfd = dst->fd;
	int infd = src->fd;

	if(need < run)
		run = need;

	while(done < need) {
		if((ret = sys_sendfile(outfd, infd, NULL, run)) <= 0)
			break;
		done += ret;
	};

	if(ret >= 0)
		return 0;
	if(!done && ret == -EINVAL)
		return -1;

	failat("sendfile", dst->dir, dst->name, ret);
}

static void alloc_rw_buf(CTX)
{
	if(ctx->buf)
		return;

	char* buf = (char*)sys_brk(0);
	char* end = (char*)sys_brk(buf + RWBUFSIZE);

	ctx->buf = buf;
	ctx->len = end - buf;
}

static void readwrite(CCT, DST, SRC, uint64_t* size)
{
	struct top* ctx = cct->top;

	alloc_rw_buf(ctx);

	uint64_t need = *size;
	uint64_t done = 0;

	char* buf = ctx->buf;
	long len = ctx->len;

	if(len > need)
		len = need;

	int rd = 0, wr;
	int rfd = src->fd;
	int wfd = dst->fd;

	while(done < need) {
		if((rd = sys_read(rfd, buf, len)) <= 0)
			break;
		if((wr = writeall(wfd, buf, rd)) < 0)
			failat("write", dst->dir, dst->name, wr);
		done += rd;
	} if(rd < 0) {
		failat("read", src->dir, src->name, rd);
	}
}

static void transfer(CCT, DST, SRC, uint64_t* size)
{
	if(!cct->nosf)
		goto rw;

	if(sendfile(cct, dst, src, size) >= 0)
		return;
rw:
	cct->nosf = 1;

	readwrite(cct, dst, src, size);
}

/* Tree walking routines (the core of the tool) */

static void scan_directory(CCT)
{
	int rd, fd = cct->src.at;
	char buf[1024];

	while((rd = sys_getdents(fd, buf, sizeof(buf))) > 0) {
		char* p = buf;
		char* e = buf + rd;

		while(p < e) {
			struct dirent* de = (struct dirent*) p;

			p += de->reclen;

			if(dotddot(de->name))
				continue;

			copy(cct, de->name, de->name, de->type);
		}
	}
}

static int open_directory(int at, char* dir, char* name)
{
	int fd;

	if((fd = sys_openat(at, name, O_DIRECTORY)) < 0)
		failat(NULL, dir, name, fd);

	return fd;
}

static int open_creat_dir(int at, char* dir, char* name, int mode)
{
	int ret;

	if((ret = sys_openat(at, name, O_DIRECTORY)) >= 0)
		return ret;
	else if(ret != -ENOENT)
		goto err;
	if((ret = sys_mkdirat(at, name, mode)) < 0)
		goto err;
	if((ret = sys_openat(at, name, O_DIRECTORY)) >= 0)
		return ret;
err:
	failat(NULL, dir, name, ret);
}

static void directory(CCT, char* dstname, char* srcname, struct stat* st)
{
	int srcat = cct->src.at;
	int dstat = cct->dst.at;

	char* srcdir = cct->src.dir;
	char* dstdir = cct->dst.dir;

	int srcfd = open_directory(srcat, srcdir, srcname);
	int dstfd = open_creat_dir(dstat, dstdir, dstname, st->mode);

	int srclen = pathlen(srcdir, srcname);
	int dstlen = pathlen(dstdir, dstname);

	char srcpath[srclen];
	char dstpath[dstlen];

	makepath(srcpath, sizeof(srcpath), srcdir, srcname);
	makepath(dstpath, sizeof(dstpath), dstdir, dstname);

	struct cct next = {
		.top = cct->top,
		.src = { srcfd, srcpath },
		.dst = { dstfd, dstpath }
	};

	scan_directory(&next);

	sys_close(srcfd);
	sys_close(dstfd);
}

static void open_atfd(struct atfd* ff, struct stat* st, int flags, int mode)
{
	int fd, ret;
	int at = ff->at;
	char* dir = ff->dir;
	char* name = ff->name;

	int stf = AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW;

	if((fd = sys_openat4(at, name, flags, mode)) < 0)
		failat(NULL, dir, name, fd);
	if((ret = sys_fstatat(fd, "", st, stf)) < 0)
		failat("stat", dir, name, ret);

	ff->fd = fd;
}

static void prep_file_pair(DST, SRC, uint64_t* size)
{
	struct stat srcst;
	struct stat dstst;

	int sflags = O_RDONLY;
	int dflags = O_WRONLY | O_CREAT | O_TRUNC;

	open_atfd(src, &srcst, sflags, 0);
	open_atfd(dst, &dstst, dflags, srcst.mode);

	if(srcst.dev == dstst.dev && srcst.ino == dstst.ino)
		failat("copy into self:", src->dir, src->name, 0);

	*size = srcst.size;
}

static void regular(CCT, char* dstname, char* srcname, struct stat* st)
{
	/* We do *not* use st here, and to save an extra stat() call it
	   may not even be initialized. Regular files get open()ed anyway,
	   so we do fstat(fd) instead to get their properties. */

	struct atfd src = {
		.at = cct->src.at,
		.dir = cct->src.dir,
		.name = srcname
	};

	struct atfd dst = {
		.at = cct->dst.at,
		.dir = cct->dst.dir,
		.name = dstname
	};

	uint64_t size;

	prep_file_pair(&dst, &src, &size);

	transfer(cct, &dst, &src, &size);

	sys_close(dst.fd);
	sys_close(src.fd);
}

static void symlink(CCT, char* dstname, char* srcname, struct stat* srcst)
{
	int srcat = cct->src.at;
	int dstat = cct->dst.at;

	char* srcdir = cct->src.dir;
	char* dstdir = cct->dst.dir;

	if(srcst->size <= 0 || srcst->size > 4096)
		failat(NULL, srcdir, srcname, -EINVAL);

	long len = srcst->size + 5;
	char buf[len];
	int ret;

	if((ret = sys_readlinkat(srcat, srcname, buf, len)) < 0)
		failat("readlink", srcdir, srcname, ret);

	buf[ret] = '\0';

	if((ret = sys_symlinkat(buf, dstat, dstname)) >= 0)
		return;
	if(ret != -EEXIST)
		goto err;
	if((ret = sys_unlinkat(dstat, dstname, 0)) < 0)
		failat("unlink", dstdir, dstname, ret);
	if((ret = sys_symlinkat(buf, dstat, dstname)) >= 0)
		return;
err:
	failat("symlink", dstdir, dstname, ret);
}

static int stifmt_to_dt(struct stat* st)
{
	switch(st->mode & S_IFMT) {
		case S_IFREG: return DT_REG;
		case S_IFDIR: return DT_DIR;
		case S_IFLNK: return DT_LNK;
		default: return DT_UNKNOWN;
	}
}

static void copy(CCT, char* dstname, char* srcname, int type)
{
	int srcat = cct->src.at;
	char* srcdir = cct->src.dir;
	int flags = AT_SYMLINK_NOFOLLOW;

	int ret;
	struct stat st;

	if(type == DT_REG)
		memzero(&st, sizeof(st));
	else if((ret = sys_fstatat(srcat, srcname, &st, flags)) < 0)
		failat(NULL, srcdir, srcname, ret);
	else
		type = stifmt_to_dt(&st);

	switch(type) {
		case DT_DIR: return directory(cct, dstname, srcname, &st);
		case DT_REG: return regular(cct, dstname, srcname, &st);
		case DT_LNK: return symlink(cct, dstname, srcname, &st);
	}

	warnat("ignoring", srcdir, srcname, 0);
}

/* Arguments parsing and tree walker invocation */

static int got_args(CTX)
{
	return ctx->argi < ctx->argc;
}

static void need_some_args(CTX)
{
	if(!got_args(ctx))
		fail("too few arguments", NULL, 0);
}

static void no_more_args(CTX)
{
	if(got_args(ctx))
		fail("too many arguments", NULL, 0);
}

static char* shift_arg(CTX)
{
	need_some_args(ctx);

	return ctx->argv[ctx->argi++];
}

static void copy_over(CTX, CCT)
{
	char* dst = shift_arg(ctx);
	char* src = shift_arg(ctx);
	no_more_args(ctx);

	if(!strcmp(dst, src))
		fail("already here:", src, 0);

	copy(cct, dst, src, DT_UNKNOWN);
}

static void copy_many(CTX, CCT)
{
	need_some_args(ctx);

	while(got_args(ctx)) {
		char* src = shift_arg(ctx);
		char* dst = basename(src);

		if(src == dst)
			fail("already here:", src, 0);

		copy(cct, dst, src, DT_UNKNOWN);
	}
}

static void open_target_dir(CTX, CCT)
{
	char* dst = shift_arg(ctx);
	int fd;

	if((fd = sys_open(dst, O_DIRECTORY)) < 0)
		fail(NULL, dst, fd);

	cct->dst.at = fd;
	cct->dst.dir = dst;
}

static int prep_opts(CTX, int argc, char** argv)
{
	int i = 1, opts = 0;

	if(i < argc && argv[i][0] == '-')
		opts = argbits(OPTS, argv[i++] + 1);

	ctx->argc = argc;
	ctx->argv = argv;
	ctx->argi = i;
	ctx->opts = opts;

	return opts;
}

int main(int argc, char** argv)
{
	struct top context, *ctx = &context;
	int opts = prep_opts(ctx, argc, argv);

	struct cct cct  = {
		.top = ctx,
		.src = { AT_FDCWD, NULL },
		.dst = { AT_FDCWD, NULL }
	};

	if(opts & OPT_h)
		;
	else if(opts & OPT_t)
		open_target_dir(ctx, &cct);

	if(opts & (OPT_t | OPT_h))
		copy_many(ctx, &cct);
	else if(opts & (OPT_n | OPT_o))
		copy_over(ctx, &cct);
	else
		fail("no mode specified", NULL, 0);

	return 0;
}