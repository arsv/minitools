#include <sys/statfs.h>
#include <sys/close.h>
#include <sys/open.h>
#include <sys/stat.h>
#include <sys/read.h>

#include <argbits.h>
#include <writeout.h>
#include <memcpy.h>
#include <strlen.h>
#include <strcmp.h>
#include <fmtpad.h>
#include <strcbrk.h>
#include <fmtlong.h>
#include <fmtint32.h>
#include <fmtstr.h>
#include <fmtchar.h>
#include <fail.h>

#define OPTS "am"
#define OPT_a (1<<0)	/* show all mounted filesystems */
#define OPT_m (1<<1)	/* show in-memory filesystems (dev id 0:*) */
#define SET_x (1<<16)	/* show systems with zero block count */

ERRTAG = "df";
ERRLIST = { RESTASNUMBERS };

char minbuf[4096]; /* mountinfo */

long xopen(const char* fname, int flags)
{
	return xchk(sysopen(fname, flags), "cannot open", fname);
}

void xwriteout(char* buf, int len)
{
	xchk(writeout(buf, len), "write", NULL);
}

static char* fmt4is(char* p, char* e, int n)
{
	if(p + 3 >= e) return e;

	*(p+3) = '0' + (n % 10); n /= 10;
	*(p+2) = n ? ('0' + n % 10) : ' '; n /= 10;
	*(p+1) = n ? ('0' + n % 10) : ' '; n /= 10;
	*(p+0) = n ? ('0' + n % 10) : ' ';

	return p + 4;
}

static char* fmt1i0(char* p, char* e, int n)
{
	if(p < e) *p++ = '0' + (n % 10);
	return p;
}

static char* fmtmem(char* p, char* e, unsigned long n, int mu)
{
	static const char sfx[] = "BKMGTP";

	/* mu is typically 4KB or so, and we can skip three extra zeros */
	int muinkb = !(mu % 1024);
	int sfi = muinkb ? 1 : 0;
	unsigned long nb = muinkb ? n*(mu/1024) : n*mu;
	unsigned long fr = 0;

	/* find out the largest multiplier we can use */
	for(; sfi < sizeof(sfx) && nb > 1024; sfi++) {
		fr = nb % 1024;
		nb /= 1024;
	}

	if(sfi >= sizeof(sfx)) {
		/* it's too large; format the number and be done with it */
		p = fmtlong(p, e, nb);
		p = fmtchar(p, e, sfx[sizeof(sfx)-1]);
	} else {
		/* it's manageable; do nnnn.d conversion */
		fr = fr*10/1024; /* one decimals */
		p = fmt4is(p, e, nb);
		p = fmtchar(p, e, '.');
		p = fmt1i0(p, e, fr);
		p = fmtchar(p, e, sfx[sfi]);
	}

	return p;
}

/* The output should look like something like this:

   Size   Used    Free         Mountpoint
   9.8G   4.5G    5.3G   47%   /

   The numbers should be aligned on decimal point. */

static void wrheader()
{
	static const char hdr[] =
	    /* |1234.1x_| */
	       "   Size "
	       "   Used "
	       "   Free"
	    /* |    12%   | */
	       "    Use   "
	       "Mountpoint\n";

	xwriteout((char*)hdr, sizeof(hdr));
}

static char* fmtstatfs(char* p, char* e, struct statfs* st, int opts)
{
	long bs = st->f_bsize;
	long blocksused = st->f_blocks - st->f_bavail;
	long blockstotal = st->f_blocks;

	p = fmtmem(p, e, st->f_blocks, bs);
	p = fmtstr(p, e, " ");
	p = fmtmem(p, e, st->f_blocks - st->f_bavail, bs);
	p = fmtstr(p, e, " ");
	p = fmtmem(p, e, st->f_bavail, bs);
	p = fmtstr(p, e, "   ");

	if(blockstotal) {
		long perc = 100*blocksused/blockstotal;
		char* q = p;
		p = fmtlong(p, e, perc);
		p = fmtstr(p, e, "%");
		p = fmtpad(q, e, 4, p);
	} else {
		p = fmtstr(p, e, "  --");
	}

	p = fmtstr(p, e, "   ");

	return p;
}

static int checkdev(char* dev, int opts)
{
	/* Major 0 means vfs. Used/free space counts are meaningless
	   for most of them. */
	int nodev = (dev[0] == '0' && dev[1] == ':');

	if(nodev && !(opts & (OPT_a | OPT_m)))
		return 0;
	if(!nodev && (opts & OPT_m))
		return 0;

	return 1;
}

static void reportfs(char* statfile, char* mountpoint, int opts)
{
	struct statfs st;

	xchk(sysstatfs(statfile, &st), "statfs", statfile);

	char* tag = mountpoint ? mountpoint : statfile;

	int len = strlen(tag) + 100;
	char buf[len];
	char* p = buf;
	char* e = buf + sizeof(buf) - 1;

	p = fmtstatfs(p, e, &st, opts);

	if(!st.f_blocks && !(opts & SET_x))
		return;

	if(mountpoint) {
		p = fmtstr(p, e, mountpoint);
	} else {
		p = fmtstr(p, e, "? ");
		p = fmtstr(p, e, statfile);
	}

	*p++ = '\n';

	xwriteout(buf, p - buf);
}

static char* findline(char* p, char* e)
{
	while(p < e)
		if(*p == '\n')
			return p;
		else
			p++;
	return NULL;
}

/* A line from mountinfo looks like this:

   66 21 8:3 / /home rw,noatime,nodiratime shared:25 - ext4 /dev/sda3 rw,stripe=128,data=ordered

   We need fields [2] devid, [4] mountpoint, and possibly [8] device.
   The fields are presumed to be space-separated, with no spaces within.
   If they aren't, we'll get garbage.

   Actually, we don't need [8] device. Too far to the right. */

#define MP 5

static int splitline(char* line, char** parts, int n)
{
	char* p = line;
	char* q;
	int i;

	for(i = 0; i < n; i++) {
		if(!*(q = strcbrk(p, ' ')))
			break;
		parts[i] = p;
		*q = '\0'; p = q + 1;
	};

	return (i < n);
}

static void scanall(char* statfile, const char* dev, int opts)
{
	long fd = xopen("/proc/self/mountinfo", O_RDONLY);
	long rd;
	long of = 0;
	char* mp[MP];

	while((rd = sysread(fd, minbuf + of, sizeof(minbuf) - of)) > 0) {
		char* p = minbuf;
		char* e = minbuf + rd;
		char* q;

		while((q = findline(p, e))) {
			char* line = p; *q = '\0'; p = q + 1;

			if(splitline(line, mp, MP))
				continue;

			char* linedev = mp[2];
			char* linemp = mp[4];

			if(!dev && checkdev(linedev, opts)) {
				reportfs(linemp, linemp, opts);
			} else if(dev && !strcmp(linedev, dev)) {
				reportfs(statfile, linemp, opts);
				goto done;
			}
		} if(p < e) {
			of = e - p;
			memcpy(minbuf, p, of);
		}
	}

	/* specific file was given but we could not find the mountpoint */
	if(dev) reportfs(statfile, NULL, opts);

done:	sysclose(fd);
}

static char* fmtdev(char* p, char* e, uint64_t dev)
{
	int min = ((dev >>  0) & 0x000000FF)
	        | ((dev >> 12) & 0xFFFFFF00);
	int maj = ((dev >>  8) & 0x00000FFF)
	        | ((dev >> 32) & 0xFFFFF000);

	p = fmti32(p, e, maj);
	p = fmtstr(p, e, ":");
	p = fmti32(p, e, min);

	return p;
}

static void scan(char* statfile, int opts)
{
	struct stat st;

	char buf[20];
	char* end = buf + sizeof(buf) - 1;

	xchk(sysstat(statfile, &st), "cannot stat", statfile);

	char* p = fmtdev(buf, end, st.st_dev); *p++ = '\0';

	scanall(statfile, buf, opts);
}

int main(int argc, char** argv)
{
	int opts = 0;
	int i = 1;

	if(i < argc && argv[i][0] == '-')
		opts = argbits(OPTS, argv[i++] + 1);

	wrheader();

	if(i >= argc)
		scanall(NULL, NULL, opts);
	else for(; i < argc; i++)
		scan(argv[i], opts | SET_x);

	flushout();

	return 0;
}
