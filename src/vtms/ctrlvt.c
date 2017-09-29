#include <bits/ioctl/tty.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/proc.h>
#include <sys/file.h>

#include <errtag.h>
#include <format.h>
#include <string.h>
#include <util.h>

ERRTAG("ctrlvt");

#define CSI "\033["

int rows;
int cols;
int initialized;
char** environ;

struct box {
	int r, c, w, h;
} box;

static struct termios tso;

void output(char* s, int len)
{
	sys_write(STDOUT, s, len);
}

void outstr(char* s)
{
	output(s, strlen(s));
}

static void tcs(char* csi, int n, int m, char c)
{
	char buf[20];
	char* p = buf;
	char* e = buf + sizeof(buf) - 1;

	p = fmtstr(p, e, csi);

	if(n)
		p = fmtint(p, e, n);
	if(n && m)
		p = fmtstr(p, e, ";");
	if(m)
		p = fmtint(p, e, m);

	p = fmtchar(p, e, c);
	*p = '\0';

	output(buf, p - buf);
}

void moveto(int r, int c)
{
	tcs(CSI, r, c, 'H');
}

void park_cursor(void)
{
	moveto(rows, 1);
}

void hide_cursor(void)
{
	tcs(CSI "?", 25, 0, 'l');
}

void show_cursor(void)
{
	tcs(CSI "?", 25, 0, 'h');
}

void clear(void)
{
	moveto(1,1);
	tcs(CSI, 0, 0, 'J');
}

void term_init(void)
{
	struct termios ts;
	struct winsize ws;
	int ret;

	if((ret = sys_ioctl(0, TIOCGWINSZ, &ws)) < 0)
		fail("ioctl", "TIOCGWINSZ", ret);

	rows = ws.row;
	cols = ws.col;

	if((ret = sys_ioctl(0, TCGETS, &ts)) < 0)
		fail("ioctl", "TCGETS", ret);

	memcpy(&tso, &ts, sizeof(ts));
	ts.iflag |= IUTF8;
	ts.iflag &= ~(IGNPAR | ICRNL | IXON | IMAXBEL);
	ts.oflag &= ~(OPOST | ONLCR);
	ts.lflag &= ~(ICANON | ECHO);

	if((ret = sys_ioctl(0, TCSETS, &ts)) < 0)
		fail("ioctl", "TCSETS", ret);

	initialized = 1;

	hide_cursor();
}

void term_back(void)
{
	struct termios ts;
	int ret;

	memcpy(&ts, &tso, sizeof(ts));

	ts.iflag |= IUTF8;
	ts.iflag &= ~(IGNPAR | ICRNL | IXON | IMAXBEL);
	ts.oflag &= ~(OPOST | ONLCR);
	ts.lflag &= ~(ICANON | ECHO);

	if((ret = sys_ioctl(0, TCSETS, &ts)) < 0)
		fail("ioctl", "TCSETS", ret);

	initialized = 1;

	hide_cursor();
}

void term_fini(void)
{
	int ret;

	if(!initialized)
		return;

	park_cursor();
	show_cursor();

	if((ret = sys_ioctl(0, TCSETS, &tso)) < 0)
		fail("ioctl", "TCSETS", ret);

	initialized = 0;
}

static int spawn(char* cmd, char* arg)
{
	int pid, status, ret;

	if((pid = sys_fork()) < 0) {
		warn("fork", NULL, pid);
		return -1;
	}

	if(pid == 0) {
		char* argv[] = { cmd, arg, NULL };
		ret = execvpe(*argv, argv, environ);
		warn("exec", *argv, ret);
		_exit(ret ? -1 : 0);
	}

	if((ret = sys_waitpid(pid, &status, 0)) < 0) {
		warn("wait", NULL, ret);
		return -1;
	}

	return status;
}

static int enter_sleep_mode(void)
{
	int fd;
	char* name = "/sys/power/state";

	if((fd = sys_open(name, O_WRONLY)) < 0) {
		warn(NULL, name, fd);
		return -1;
	}

	sys_write(fd, "mem\n", 4);

	sys_close(fd);

	return 0;
}

static void cmd_reboot(void)
{
	term_fini();
	spawn("svctl", "-R");
	term_back();
}

static void cmd_poweroff(void)
{
	term_fini();
	spawn("svctl", "-P");
	term_back();
}

static void cmd_sleep(void)
{
	term_fini();
	enter_sleep_mode();
	term_back();
}

static void cmd_lock(void)
{

}

static void cmd_exit(void)
{
	term_fini();
	_exit(0);
}

static void cmd_back(void)
{
	int ret;

	term_fini();

	if(!(ret = spawn("vtctl", "-b")))
		_exit(0);

	term_back();
}

static void promp_action(void)
{
	char buf[40];
	int ret;

	char* prompt = "[Esc]back  [R]eboot  [P]oweroff  [S]leep  [L]ock";
	int plen = strlen(prompt);

	int r = rows/2;
	int c = cols/2 - plen/2;
redraw:
	clear();
	moveto(r, c);
	outstr(prompt);

	while((ret = sys_read(STDIN, buf, sizeof(buf))) > 0)
		if(ret != 1)
			continue;
		else switch(buf[0]) {
			case '\004': cmd_exit(); break;
			case '\033': cmd_back(); break;
			case '\014': goto redraw;
			case 'R': cmd_reboot(); break;
			case 'P': cmd_poweroff(); break;
			case 'S': cmd_sleep(); break;
			case 'L': cmd_lock(); break;
		}
}

static void sighandler(int _)
{
	term_fini();
	_exit(0);
}

static void setup_signals(void)
{
	struct sigaction sa = {
		.handler = sighandler,
		.flags = SA_RESTORER,
		.restorer = sigreturn
	};

	sys_sigaction(SIGTERM, &sa, NULL);
	sys_sigaction(SIGINT,  &sa, NULL);
}

int main(int argc, char** argv, char** envp)
{
	environ = envp;

	setup_signals();
	term_init();

	tcs(CSI, rows/2 + 5, rows, 'r');

	promp_action();

	return -1;
}