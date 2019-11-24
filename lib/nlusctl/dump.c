#include <nlusctl.h>
#include <format.h>
#include <string.h>
#include <util.h>

static void dump_attr(char* pref, struct ucattr* at);

static void dump_rec(char* pref, struct ucattr* bt)
{
	struct ucattr* at;

	FMTBUF(p, e, npf, strlen(pref) + 10);
	p = fmtstr(p, e, "  ");
	p = fmtstr(p, e, pref);
	FMTEND(p, e);

	for(at = uc_sub_0(bt); at; at = uc_sub_n(bt, at))
		dump_attr(npf, at);
}

static char* tag(char* p, char* e, char* pref, struct ucattr* at, char* type)
{
	p = fmtstr(p, e, pref);
	p = fmtstr(p, e, "ATTR ");
	p = fmtint(p, e, at->key);
	p = fmtstr(p, e, type);
	return p;
}

static void output(char* p, char* e, char* buf)
{
	FMTENL(p, e);
	writeall(STDERR, buf, p - buf);
}

static void at_empty(char* pref, struct ucattr* at)
{
	FMTBUF(p, e, buf, 50);
	p = tag(p, e, pref, at, " empty");
	output(p, e, buf);
}

static void at_nest(char* pref, struct ucattr* at)
{
	FMTBUF(p, e, buf, 50);
	p = tag(p, e, pref, at, " nest");
	output(p, e, buf);

	dump_rec(pref, at);
}

//void at_string(char* pref, struct ucattr* at, char* str)
//{
//	char* q;
//	byte c;
//
//	FMTBUF(p, e, buf, 30 + strlen(str));
//	p = tag(p, e, pref, at, " ");
//	p = fmtstr(p, e, "\"");
//
//	for(q = str; (c = *q); q++) {
//		if(c >= 0x20 && c < 0x7F) {
//			p = fmtchar(p, e, c);
//		} else {
//			p = fmtstr(p, e, "\\x");
//			p = fmtbyte(p, e, c);
//		}
//	}
//
//	p = fmtstr(p, e, "\"");
//	output(p, e, buf);
//}
//
//void at_int(char* pref, struct ucattr* at, int* val)
//{
//	FMTBUF(p, e, buf, 50);
//	p = tag(p, e, pref, at, " int ");
//	p = fmtstr(p, e, "0x");
//	p = fmtpad0(p, e, 8, fmthex(p, e, *val));
//	p = fmtstr(p, e, " ");
//	p = fmtint(p, e, *val);
//	output(p, e, buf);
//}

static void at_raw(char* pref, struct ucattr* at, void* data, int dlen)
{
	char* q = data;
	int i;

	FMTBUF(p, e, buf, 80);
	p = tag(p, e, pref, at, " raw");

	for(i = 0; i < dlen; i++) {
		p = fmtstr(p, e, " ");
		p = fmtbyte(p, e, q[i]);
	}

	output(p, e, buf);
}

static void at_trash(char* pref, struct ucattr* at, void* data, int dlen)
{
	char* q = data;
	int i;

	FMTBUF(p, e, buf, 80);
	p = tag(p, e, pref, at, "");

	for(i = 0; i < dlen; i++) {
		if(i % 16 == 0) {
			output(p, e, buf);
			p = buf;
			p = fmtstr(p, e, pref);
			p = fmtstr(p, e, " ");
		}
		p = fmtstr(p, e, " ");
		p = fmtbyte(p, e, q[i]);
	}

	output(p, e, buf);
}

static void dump_attr(char* pref, struct ucattr* at)
{
	int paylen = uc_paylen(at);
	void* payload = uc_payload(at);
	int key = at->key;

	if(paylen == 0)
		at_empty(pref, at);
	else if(uc_is_nest(at, key))
		at_nest(pref, at);
	else if(paylen < 15)
		at_raw(pref, at, payload, paylen);
	else
		at_trash(pref, at, payload, paylen);
}


static void dump_attrs_in(struct ucmsg* msg)
{
	struct ucattr* at;

	for(at = uc_get_0(msg); at; at = uc_get_n(msg, at))
		dump_attr("  ", at);
}

static int printable(int c)
{
	return (c > 0x20 && c < 0x7F);
}

static void dump_header(struct ucmsg* msg)
{
	int cmd = msg->cmd;
	int paylen = msg->len - sizeof(*msg);

	FMTBUF(p, e, buf, 80);

	p = fmtstr(p, e, "NLUS");

	if(cmd < 0) {
		p = fmtstr(p, e, " error ");
		p = fmtint(p, e, -cmd);
	} else {
		int c1 = (cmd >> 24) & 0xFF;
		int c2 = (cmd >> 16) & 0xFF;

		if(printable(c1) && printable(c2)) {
			p = fmtstr(p, e, " ");
			p = fmtchar(p, e, c1);
			p = fmtchar(p, e, c2);
			p = fmtstr(p, e, "(");
			p = fmtint(p, e, cmd & 0xFFFF);
			p = fmtstr(p, e, ")");
		} else {
			p = fmtstr(p, e, " cmd=");
			p = fmtint(p, e, cmd);
		}
	}

	if(paylen > 0) {
		p = fmtstr(p, e, " payload ");
		p = fmtint(p, e, paylen);
	};

	FMTENL(p, e);

	writeall(STDERR, buf, p - buf);
}

void uc_dump(struct ucmsg* msg)
{
	dump_header(msg);
	dump_attrs_in(msg);
}

void uc_dump_rx(struct urbuf* ur)
{
	uc_dump(ur->msg);
}

void uc_dump_tx(struct ucbuf* uc)
{
	uc_dump((struct ucmsg*)uc->buf);
}
