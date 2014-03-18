/* See LICENSE for licence details. */
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/X.h> // XXX

#include <fontconfig/fontconfig.h>

#if   defined(__linux)
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#endif

#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)

#include "term.h"

/* Selection code */

static void selscroll(struct st_term *term, int orig, int n)
{
	struct st_selection *sel = &term->sel;

	if (sel->type == SEL_NONE)
		return;

	if (BETWEEN(sel->p1.y, orig, term->bot) ||
	    BETWEEN(sel->p2.y, orig, term->bot)) {
		if ((sel->p1.y += n) > term->bot ||
		    (sel->p2.y += n) < term->top) {
			sel->type = SEL_NONE;
			return;
		}

		switch (sel->type) {
		case SEL_NONE:
			break;
		case SEL_REGULAR:
			if (sel->p1.y < term->top) {
				sel->p1.y = term->top;
				sel->p1.x = 0;
			}
			if (sel->p2.y > term->bot) {
				sel->p2.y = term->bot;
				sel->p2.x = term->size.y;
			}
			break;
		case SEL_RECTANGULAR:
			if (sel->p1.y < term->top)
				sel->p1.y = term->top;
			if (sel->p2.y > term->bot)
				sel->p2.y = term->bot;
			break;
		};
	}
}

bool term_selected(struct st_selection *sel, int x, int y)
{
	switch (sel->type) {
	case SEL_NONE:
		return false;
	case SEL_REGULAR:
		if (y < sel->p1.y || y > sel->p2.y)
			return false;

		if (y == sel->p1.y && x < sel->p1.x)
			return false;

		if (y == sel->p2.y && x > sel->p2.x)
			return false;

		return true;
	case SEL_RECTANGULAR:
		return sel->p1.y <= y && y <= sel->p2.y &&
			sel->p1.x <= x && x <= sel->p2.x;
	}

	return false;
}

static void term_sel_copy(struct st_term *term)
{
	struct st_selection *sel = &term->sel;
	unsigned char *str = NULL, *ptr;

	if (sel->type == SEL_NONE)
		goto out;

	ptr = str = xmalloc((sel->p2.y - sel->p1.y + 1) *
			    term->size.y * UTF_SIZ);

	/* append every set & selected glyph to the selection */
	for (unsigned y = sel->p1.y; y <= sel->p2.y; y++) {
		struct st_glyph *gp = &term->line[y][0];
		struct st_glyph *last = &term->line[y][term->size.x - 1];

		if (sel->type == SEL_RECTANGULAR ||
		    y == sel->p1.y)
			gp = &term->line[y][sel->p1.x];

		if (sel->type == SEL_RECTANGULAR ||
		    y == sel->p2.y)
			last = &term->line[y][sel->p2.x];

		while (last > gp && !last->c)
			last--;

		for (; gp <= last; gp++) {
			int ret = FcUcs4ToUtf8(gp->c, ptr);
			if (ret > 0)
				ptr += ret;
			else
				*ptr++ = ' ';
		}

		/* \n at the end of every selected line except for the last one */
		if (y < sel->p2.y)
			*ptr++ = '\r';
	}
	*ptr = 0;
out:
	free(sel->clip);
	sel->clip = (char *) str;
}

void term_sel_update(struct st_term *term, unsigned type,
		     struct coord start, struct coord end)
{
	struct st_selection *sel = &term->sel;

	sel->p1 = start;
	sel->p2 = end;

	switch (sel->type) {
	case SEL_NONE:
		sel->type = type;
		break;
	case SEL_REGULAR:
		if (sel->p1.y > sel->p2.y ||
		    (sel->p1.y == sel->p2.y &&
		     sel->p1.x > sel->p2.x))
			swap(sel->p1, sel->p2);

		term_sel_copy(term);
		break;
	case SEL_RECTANGULAR:
		if (sel->p1.x > sel->p2.x)
			swap(sel->p1.x, sel->p2.x);
		if (sel->p1.y > sel->p2.y)
			swap(sel->p1.y, sel->p2.y);

		term_sel_copy(term);
		break;
	}

	term->dirty = true;
}

static bool isword(unsigned c)
{
	static const char not_word[] = "*.!?;=&#$%^[](){}<>";

	return c && !isspace(c) && !strchr(not_word, c);
}

void term_sel_word(struct st_term *term, struct coord pos)
{
	struct coord start = pos;

	while (start.x &&
	       isword(term_pos(term, start)[-1].c))
		start.x--;

	while (pos.x < term->size.x - 1 &&
	       isword(term_pos(term, pos)[1].c))
		pos.x++;

	term_sel_update(term, SEL_REGULAR, start, pos);
}

void term_sel_line(struct st_term *term, struct coord pos)
{
	struct coord start = pos;

	start.x = 0;
	pos.x = term->size.x - 1;

	term_sel_update(term, SEL_REGULAR, start, pos);
}

void term_sel_stop(struct st_term *term)
{
	term->sel.type = SEL_NONE;
	term->dirty = true;
}

/* Escape handling */

static void csiparse(struct csi_escape *csi)
{
	char *p = csi->buf, *np;
	long int v;

	csi->narg = 0;
	if (*p == '?') {
		csi->priv = 1;
		p++;
	}

	csi->buf[csi->len] = '\0';
	while (p < csi->buf + csi->len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csi->arg[csi->narg++] = v;
		p = np;
		if (*p != ';' || csi->narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csi->mode = *p;
}

static void csidump(struct csi_escape *csi)
{
	int i;
	unsigned c;

	printf("ESC[");
	for (i = 0; i < csi->len; i++) {
		c = csi->buf[i] & 0xff;
		if (isprint(c)) {
			putchar(c);
		} else if (c == '\n') {
			printf("(\\n)");
		} else if (c == '\r') {
			printf("(\\r)");
		} else if (c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	putchar('\n');
}

static void csireset(struct csi_escape *csi)
{
	memset(csi, 0, sizeof(*csi));
}

/* t code */

static void __tclearline(struct st_term *term, unsigned y,
			 unsigned start, unsigned end)
{
	struct st_glyph *g;

	term->dirty = true;

	for (g = &term->line[y][start];
	     g < &term->line[y][end];
	     g++)
		*g = term->c.attr;
}

static void tclearline(struct st_term *term, unsigned start, unsigned end)
{
	__tclearline(term, term->c.pos.y, start, end);
}

static void tclearregion(struct st_term *term, struct coord p1,
			 struct coord p2)
{
	struct coord p;

	for (p.y = p1.y; p.y < p2.y; p.y++)
		__tclearline(term, p.y, p1.x, p2.x);
}

static void tscrolldown(struct st_term *term, int orig, int n)
{
	int i;

	n = clamp_t(int, n, 0, term->bot - orig + 1);

	tclearregion(term,
		     (struct coord) {0, term->bot - n + 1},
		     (struct coord) {term->size.x, term->bot + 1});

	for (i = term->bot; i >= orig + n; i--)
		swap(term->line[i], term->line[i - n]);

	selscroll(term, orig, n);
}

static void tscrollup(struct st_term *term, int orig, int n)
{
	int i;

	n = clamp_t(int, n, 0, term->bot - orig + 1);

	tclearregion(term,
		     (struct coord) {0, orig},
		     (struct coord) {term->size.x, orig + n});

	/* XXX: optimize? */
	for (i = orig; i <= term->bot - n; i++)
		swap(term->line[i], term->line[i + n]);

	selscroll(term, orig, -n);
}

static void tmovex(struct st_term *term, unsigned x)
{
	term->dirty = true;

	term->c.wrapnext = 0;
	term->c.pos.x = min(x, term->size.x - 1);
}

static void tmovey(struct st_term *term, unsigned y)
{
	term->dirty = true;

	term->c.wrapnext = 0;
	term->c.pos.y = term->c.origin
		? clamp(y, term->top, term->bot)
		: min(y, term->size.y - 1);
}

static void tmoveto(struct st_term *term, struct coord pos)
{
	tmovex(term, pos.x);
	tmovey(term, pos.y);
}

/* for absolute user moves, when decom is set */
static void tmoveato(struct st_term *term, struct coord pos)
{
	if (term->c.origin)
		pos.y += term->top;

	tmoveto(term, pos);
}

static void tmoverel(struct st_term *term, int x, int y)
{
	term->dirty = true;

	term->c.pos.x = clamp_t(int, term->c.pos.x + x, 0, term->size.x - 1);
	term->c.pos.y = clamp_t(int, term->c.pos.y + y, 0, term->size.y - 1);

	if (term->c.origin)
		term->c.pos.y = clamp(term->c.pos.y, term->top, term->bot);

	term->c.wrapnext = 0;
}

static void tcursor_save(struct st_term *term)
{
	term->saved = term->c;
}

static void tcursor_load(struct st_term *term)
{
	term->c = term->saved;
	tmoveto(term, term->c.pos);
}

static void treset(struct st_term *term)
{
	memset(&term->c, 0, sizeof(term->c));
	term->c.attr.cmp = 0;
	term->c.attr.fg = term->defaultfg;
	term->c.attr.bg = term->defaultbg;

	memset(term->tabs, 0, term->size.x * sizeof(*term->tabs));
	for (unsigned i = SPACES_PER_TAB; i < term->size.x; i += SPACES_PER_TAB)
		term->tabs[i] = 1;
	term->top = 0;
	term->bot = term->size.y - 1;

	term->wrap	= 1;
	term->insert	= 0;
	term->appkeypad = 0;
	term->altscreen = 0;
	term->crlf	= 0;
	term->mousebtn	= 0;
	term->mousemotion = 0;
	term->reverse	= 0;
	term->kbdlock	= 0;
	term->hide	= 0;
	term->echo	= 0;
	term->appcursor	= 0;
	term->mousesgr	= 0;

	tclearregion(term, ORIGIN, term->size);
	tmoveto(term, ORIGIN);
	tcursor_save(term);
}

static void tputtab(struct st_term *term, bool forward)
{
	struct coord pos = term->c.pos;

	if (forward) {
		if (pos.x == term->size.x)
			return;
		for (++pos.x;
		     pos.x < term->size.x && !term->tabs[pos.x];
		     ++pos.x)
			/* nothing */ ;
	} else {
		if (pos.x == 0)
			return;
		for (--pos.x;
		     pos.x > 0 && !term->tabs[pos.x];
		     --pos.x)
			/* nothing */ ;
	}
	tmoveto(term, pos);
}

static void tnewline(struct st_term *term, int first_col)
{
	struct coord pos = term->c.pos;

	if (first_col)
		pos.x = 0;

	if (pos.y == term->bot)
		tscrollup(term, term->top, 1);
	else
		pos.y++;

	tmoveto(term, pos);
}

static void tsetchar(struct st_term *term, unsigned c, struct coord pos)
{
	static const char *vt100_0[62] = {	/* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃",	/* A - G */
		0, 0, 0, 0, 0, 0, 0, 0,	/* H - O */
		0, 0, 0, 0, 0, 0, 0, 0,	/* P - W */
		0, 0, 0, 0, 0, 0, 0, " ",	/* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±",	/* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺",	/* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬",	/* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·",	/* x - ~ */
	};

	struct st_glyph *g = term_pos(term, pos);

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if (term->c.attr.gfx)
		if (c >= 0x41 && c <= 0x7e && vt100_0[c - 0x41])
			c = *vt100_0[c - 0x41];

	term->dirty = true;
	*g = term->c.attr;
	g->c = c;
}

static void tdeletechar(struct st_term *term, int n)
{
	struct coord src = term->c.pos;
	unsigned size, start = term->c.pos.x;

	src.x += n;
	size = term->size.x - src.x;

	if (src.x < term->size.x) {
		memmove(term_pos(term, term->c.pos),
			term_pos(term, src),
			size * sizeof(struct st_glyph));

		start = term->size.x - n;
	}

	tclearline(term, start, term->size.x);
}

static void tinsertblank(struct st_term *term, int n)
{
	struct coord dst = term->c.pos;
	unsigned size, end = term->size.x;

	dst.x += n;
	size = term->size.x - dst.x;

	if (dst.x < term->size.x) {
		memmove(term_pos(term, dst),
			term_pos(term, term->c.pos),
			size * sizeof(struct st_glyph));

		end = dst.x;
	}

	tclearline(term, term->c.pos.x, end);
}

static void tinsertblankline(struct st_term *term, int n)
{
	if (term->c.pos.y < term->top || term->c.pos.y > term->bot)
		return;

	tscrolldown(term, term->c.pos.y, n);
}

static void tdeleteline(struct st_term *term, int n)
{
	if (term->c.pos.y < term->top || term->c.pos.y > term->bot)
		return;

	tscrollup(term, term->c.pos.y, n);
}

static void tsetattr(struct st_term *term, int *attr, int l)
{
	int i;
	struct st_glyph *g = &term->c.attr;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			g->reverse	= 0;
			g->underline	= 0;
			g->bold		= 0;
			g->italic	= 0;
			g->blink	= 0;
			g->fg		= term->defaultfg;
			g->bg		= term->defaultbg;
			break;
		case 1:
			g->bold		= 1;
			break;
		case 3:
			g->italic	= 1;
			break;
		case 4:
			g->underline	= 1;
			break;
		case 5:	/* slow blink */
		case 6:	/* rapid blink */
			g->blink	= 1;
			break;
		case 7:
			g->reverse	= 1;
			break;
		case 21:
		case 22:
			g->bold		= 0;
			break;
		case 23:
			g->italic	= 0;
			break;
		case 24:
			g->underline	= 0;
			break;
		case 25:
		case 26:
			g->blink	= 0;
			break;
		case 27:
			g->reverse	= 0;
			break;
		case 38:
			if (i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if (BETWEEN(attr[i], 0, 255)) {
					term->c.attr.fg = attr[i];
				} else {
					fprintf(stderr,
						"erresc: bad fgcolor %d\n",
						attr[i]);
				}
			} else {
				fprintf(stderr,
					"erresc(38): gfx attr %d unknown\n",
					attr[i]);
			}
			break;
		case 39:
			term->c.attr.fg = term->defaultfg;
			break;
		case 48:
			if (i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if (BETWEEN(attr[i], 0, 255)) {
					term->c.attr.bg = attr[i];
				} else {
					fprintf(stderr,
						"erresc: bad bgcolor %d\n",
						attr[i]);
				}
			} else {
				fprintf(stderr,
					"erresc(48): gfx attr %d unknown\n",
					attr[i]);
			}
			break;
		case 49:
			term->c.attr.bg = term->defaultbg;
			break;
		default:
			if (BETWEEN(attr[i], 30, 37)) {
				term->c.attr.fg = attr[i] - 30;
			} else if (BETWEEN(attr[i], 40, 47)) {
				term->c.attr.bg = attr[i] - 40;
			} else if (BETWEEN(attr[i], 90, 97)) {
				term->c.attr.fg = attr[i] - 90 + 8;
			} else if (BETWEEN(attr[i], 100, 107)) {
				term->c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]), csidump(&term->csiescseq);
			}
			break;
		}
	}
}

static void tsetscroll(struct st_term *term, unsigned t, unsigned b)
{
	t = min(t, term->size.y - 1);
	b = min(b, term->size.y - 1);

	if (t > b)
		swap(t, b);

	term->top = t;
	term->bot = b;
}

static void tswapscreen(struct st_term *term)
{
	swap(term->line, term->alt);
	term->sel.type = SEL_NONE;
	term->altscreen ^= 1;
	term->dirty = true;
}

static void tsetmode(struct st_term *term, bool priv,
		     bool set, int *args, int narg)
{
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))
	int *lim;

	for (lim = args + narg; args < lim; ++args) {
		if (priv) {
			switch (*args) {
				break;
			case 1:	/* DECCKM -- Cursor key */
				term->appcursor = set;
				break;
			case 5:	/* DECSCNM -- Reverse video */
				if (set != term->reverse) {
					term->reverse = set;
					term->dirty = true;
				}
				break;
			case 6:	/* DECOM -- Origin */
				term->c.origin = set;
				tmoveato(term, ORIGIN);
				break;
			case 7:	/* DECAWM -- Auto wrap */
				term->wrap = set;
				break;
			case 0:	/* Error (IGNORED) */
			case 2:	/* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:	/* DECCOLM -- Column  (IGNORED) */
			case 4:	/* DECSCLM -- Scroll (IGNORED) */
			case 8:	/* DECARM -- Auto repeat (IGNORED) */
			case 18:	/* DECPFF -- Printer feed (IGNORED) */
			case 19:	/* DECPEX -- Printer extent (IGNORED) */
			case 42:	/* DECNRCM -- National characters (IGNORED) */
			case 12:	/* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25:	/* DECTCEM -- Text Cursor Enable Mode */
				term->hide = !set;
				break;
			case 1000:	/* 1000,1002: enable xterm mouse report */
				term->mousebtn = set;
				term->mousemotion = 0;
				break;
			case 1002:
				term->mousemotion = set;
				term->mousebtn = 0;
				break;
			case 1006:
				term->mousesgr = set;
				break;
			case 1049:	/* = 1047 and 1048 */
			case 47:
			case 1047:{
					if (term->altscreen)
						tclearregion(term, ORIGIN,
							     term->size);
					if (set != term->altscreen)
						tswapscreen(term);
					if (*args != 1049)
						break;
				}
				/* pass through */
			case 1048:
				if (set)
					tcursor_save(term);
				else
					tcursor_load(term);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch (*args) {
			case 0:	/* Error (IGNORED) */
				break;
			case 2:	/* KAM -- keyboard action */
				term->kbdlock = set;
				break;
			case 4:	/* IRM -- Insertion-replacement */
				term->insert = set;
				break;
			case 12:	/* SRM -- Send/Receive */
				term->echo = !set;
				break;
			case 20:	/* LNM -- Linefeed/new line */
				term->crlf = set;
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
#undef MODBIT
}

static void csihandle(struct st_term *term)
{
	struct csi_escape *csi = &term->csiescseq;

	switch (csi->mode) {
	default:
	      unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump(csi);
		/* die(""); */
		break;
	case '@':		/* ICH -- Insert <n> blank char */
		DEFAULT(csi->arg[0], 1);
		tinsertblank(term, csi->arg[0]);
		break;
	case 'A':		/* CUU -- Cursor <n> Up */
		DEFAULT(csi->arg[0], 1);
		tmoverel(term, 0, -csi->arg[0]);
		break;
	case 'B':		/* CUD -- Cursor <n> Down */
	case 'e':		/* VPR --Cursor <n> Down */
		DEFAULT(csi->arg[0], 1);
		tmoverel(term, 0, csi->arg[0]);
		break;
	case 'c':		/* DA -- Device Attributes */
		if (csi->arg[0] == 0)
			ttywrite(term, VT102ID, sizeof(VT102ID) - 1);
		break;
	case 'C':		/* CUF -- Cursor <n> Forward */
	case 'a':		/* HPR -- Cursor <n> Forward */
		DEFAULT(csi->arg[0], 1);
		tmoverel(term, csi->arg[0], 0);
		break;
	case 'D':		/* CUB -- Cursor <n> Backward */
		DEFAULT(csi->arg[0], 1);
		tmoverel(term, -csi->arg[0], 0);
		break;
	case 'E':		/* CNL -- Cursor <n> Down and first col */
		DEFAULT(csi->arg[0], 1);
		tmoverel(term, 0, csi->arg[0]);
		term->c.pos.x = 0;
		break;
	case 'F':		/* CPL -- Cursor <n> Up and first col */
		DEFAULT(csi->arg[0], 1);
		tmoverel(term, 0, -csi->arg[0]);
		term->c.pos.x = 0;
		break;
	case 'g':		/* TBC -- Tabulation clear */
		switch (csi->arg[0]) {
		case 0:	/* clear current tab stop */
			term->tabs[term->c.pos.x] = 0;
			break;
		case 3:	/* clear all the tabs */
			memset(term->tabs, 0,
			       term->size.x * sizeof(*term->tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G':		/* CHA -- Move to <col> */
	case '`':		/* HPA */
		DEFAULT(csi->arg[0], 1);
		tmovex(term, csi->arg[0] - 1);
		break;
	case 'H':		/* CUP -- Move to <row> <col> */
	case 'f':		/* HVP */
		DEFAULT(csi->arg[0], 1);
		DEFAULT(csi->arg[1], 1);
		tmoveato(term, (struct coord)
			 {csi->arg[1] - 1, csi->arg[0] - 1});
		break;
	case 'I':		/* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(csi->arg[0], 1);
		while (csi->arg[0]--)
			tputtab(term, 1);
		break;
	case 'J':		/* ED -- Clear screen */
		term->sel.type = SEL_NONE;
		switch (csi->arg[0]) {
		case 0:	/* below */
			tclearregion(term, term->c.pos, term->size);
			if (term->c.pos.y < term->size.y - 1)
				tclearregion(term, (struct coord)
					     {0, term->c.pos.y + 1},
					     term->size);
			break;
		case 1:	/* above */
			if (term->c.pos.y > 1)
				tclearregion(term, ORIGIN, term->size);
			tclearline(term, 0, term->c.pos.x + 1);
			break;
		case 2:	/* all */
			tclearregion(term, ORIGIN, term->size);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K':		/* EL -- Clear line */
		switch (csi->arg[0]) {
		case 0:	/* right */
			tclearline(term, term->c.pos.x, term->size.x);
			break;
		case 1:	/* left */
			tclearline(term, 0, term->c.pos.x + 1);
			break;
		case 2:	/* all */
			tclearline(term, 0, term->size.x);
			break;
		}
		break;
	case 'S':		/* SU -- Scroll <n> line up */
		DEFAULT(csi->arg[0], 1);
		tscrollup(term, term->top, csi->arg[0]);
		break;
	case 'T':		/* SD -- Scroll <n> line down */
		DEFAULT(csi->arg[0], 1);
		tscrolldown(term, term->top, csi->arg[0]);
		break;
	case 'L':		/* IL -- Insert <n> blank lines */
		DEFAULT(csi->arg[0], 1);
		tinsertblankline(term, csi->arg[0]);
		break;
	case 'l':		/* RM -- Reset Mode */
		tsetmode(term, csi->priv, 0, csi->arg, csi->narg);
		break;
	case 'M':		/* DL -- Delete <n> lines */
		DEFAULT(csi->arg[0], 1);
		tdeleteline(term, csi->arg[0]);
		break;
	case 'X':		/* ECH -- Erase <n> char */
		DEFAULT(csi->arg[0], 1);
		tclearline(term, term->c.pos.x, term->c.pos.x + csi->arg[0]);
		break;
	case 'P':		/* DCH -- Delete <n> char */
		DEFAULT(csi->arg[0], 1);
		tdeletechar(term, csi->arg[0]);
		break;
	case 'Z':		/* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csi->arg[0], 1);
		while (csi->arg[0]--)
			tputtab(term, 0);
		break;
	case 'd':		/* VPA -- Move to <row> */
		DEFAULT(csi->arg[0], 1);
		tmoveato(term, (struct coord) {term->c.pos.x, csi->arg[0] - 1});
		break;
	case 'h':		/* SM -- Set terminal mode */
		tsetmode(term, csi->priv, 1, csi->arg, csi->narg);
		break;
	case 'm':		/* SGR -- Terminal attribute (color) */
		tsetattr(term, csi->arg, csi->narg);
		break;
	case 'r':		/* DECSTBM -- Set Scrolling Region */
		if (csi->priv) {
			goto unknown;
		} else {
			DEFAULT(csi->arg[0], 1);
			DEFAULT(csi->arg[1], term->size.y);
			tsetscroll(term, csi->arg[0] - 1,
				   csi->arg[1] - 1);
			tmoveato(term, ORIGIN);
		}
		break;
	case 's':		/* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor_save(term);
		break;
	case 'u':		/* DECRC -- Restore cursor position (ANSI.SYS) */
		tcursor_load(term);
		break;
	}
}

/* String escape handling */

static void strparse(struct str_escape *esc)
{
	char *p = esc->buf;

	esc->narg = 0;
	esc->buf[esc->len] = '\0';
	while (p && esc->narg < STR_ARG_SIZ)
		esc->args[esc->narg++] = strsep(&p, ";");
}

static void strdump(struct str_escape *esc)
{
	int i;
	unsigned c;

	printf("ESC%c", esc->type);
	for (i = 0; i < esc->len; i++) {
		c = esc->buf[i] & 0xff;
		if (c == '\0') {
			return;
		} else if (isprint(c)) {
			putchar(c);
		} else if (c == '\n') {
			printf("(\\n)");
		} else if (c == '\r') {
			printf("(\\r)");
		} else if (c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	printf("ESC\\\n");
}

static void strreset(struct str_escape *esc)
{
	memset(esc, 0, sizeof(*esc));
}

static void strhandle(struct st_term *term)
{
	struct str_escape *esc = &term->strescseq;
	char *p = NULL;
	int i, j, narg;

	strparse(esc);
	narg = esc->narg;

	switch (esc->type) {
	case ']':		/* OSC -- Operating System Command */
		switch (i = atoi(esc->args[0])) {
		case 0:
		case 1:
		case 2:
			if (narg > 1 && term->settitle)
				term->settitle(term, esc->args[1]);
			break;
		case 4:	/* color set */
			if (narg < 3)
				break;
			p = esc->args[2];
			/* fall through */
		case 104:	/* color reset, here p = NULL */
			j = (narg > 1) ? atoi(esc->args[1]) : -1;
			if (!term->setcolorname)
				break;

			if (term->setcolorname(term, j, p))
				term->dirty = true;
			else
				fprintf(stderr,
					"erresc: invalid color %s\n", p);
			break;
		default:
			fprintf(stderr, "erresc: unknown str ");
			strdump(esc);
			break;
		}
		break;
	case 'k':		/* old title set compatibility */
		if (term->settitle)
			term->settitle(term, esc->args[0]);
		break;
	case 'P':		/* DSC -- Device Control String */
	case '_':		/* APC -- Application Program Command */
	case '^':		/* PM -- Privacy Message */
	default:
		fprintf(stderr, "erresc: unknown str ");
		strdump(esc);
		/* die(""); */
		break;
	}
}

/* Input code */

static void tputc(struct st_term *term, unsigned c)
{
	bool control = c < '\x20' || c == 0177;

	/*
	 * STR sequences must be checked before anything else
	 * because it can use some control codes as part of the sequence.
	 */
	if (term->esc & ESC_STR) {
		unsigned len;
		unsigned char buf[FC_UTF8_MAX_LEN];

		switch (c) {
		case '\033':
			term->esc = ESC_START | ESC_STR_END;
			break;
		case '\a':	/* backwards compatibility to xterm */
			term->esc = 0;
			strhandle(term);
			break;
		default:
			len = FcUcs4ToUtf8(c, buf);

			if (term->strescseq.len + len <
			    sizeof(term->strescseq.buf) - 1) {
				memmove(&term->strescseq.buf[term->strescseq.len],
					buf, len);
				term->strescseq.len += len;
			} else {
				/*
				 * Here is a bug in terminals. If the user never sends
				 * some code to stop the str or esc command, then st
				 * will stop responding. But this is better than
				 * silently failing with unknown characters. At least
				 * then users will report back.
				 *
				 * In the case users ever get fixed, here is the code:
				 */
				/*
				 * term->esc = 0;
				 * strhandle();
				 */
			}
		}
		return;
	}

	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if (control) {
		switch (c) {
		case '\t':	/* HT */
			tputtab(term, 1);
			return;
		case '\b':	/* BS */
			tmoverel(term, -1, 0);
			return;
		case '\r':	/* CR */
			tmovex(term, 0);
			return;
		case '\f':	/* LF */
		case '\v':	/* VT */
		case '\n':	/* LF */
			/* go to first col if the mode is set */
			tnewline(term, term->crlf);
			return;
		case '\a':	/* BEL */
			if (term->seturgent)
				term->seturgent(term, 1);
			return;
		case '\033':	/* ESC */
			csireset(&term->csiescseq);
			term->esc = ESC_START;
			return;
		case '\016':	/* SO */
		case '\017':	/* SI */
			/*
			 * Different charsets are hard to handle. Applications
			 * should use the right alt charset escapes for the
			 * only reason they still exist: line drawing. The
			 * rest is incompatible history st should not support.
			 */
			return;
		case '\032':	/* SUB */
		case '\030':	/* CAN */
			csireset(&term->csiescseq);
			return;
		case '\005':	/* ENQ (IGNORED) */
		case '\000':	/* NUL (IGNORED) */
		case '\021':	/* XON (IGNORED) */
		case '\023':	/* XOFF (IGNORED) */
		case 0177:	/* DEL (IGNORED) */
			return;
		}
	} else if (term->esc & ESC_START) {
		if (term->esc & ESC_CSI) {
			term->csiescseq.buf[term->csiescseq.len++] = c;
			if (BETWEEN(c, 0x40, 0x7E)
			    || term->csiescseq.len >= sizeof(term->csiescseq.buf) - 1) {
				term->esc = 0;
				csiparse(&term->csiescseq);
				csihandle(term);
			}
		} else if (term->esc & ESC_STR_END) {
			term->esc = 0;
			if (c == '\\')
				strhandle(term);
		} else if (term->esc & ESC_ALTCHARSET) {
			switch (c) {
			case '0':	/* Line drawing set */
				term->c.attr.gfx = 1;
				break;
			case 'B':	/* USASCII */
				term->c.attr.gfx = 0;
				break;
			case 'A':	/* UK (IGNORED) */
			case '<':	/* multinational charset (IGNORED) */
			case '5':	/* Finnish (IGNORED) */
			case 'C':	/* Finnish (IGNORED) */
			case 'K':	/* German (IGNORED) */
				break;
			default:
				fprintf(stderr,
					"esc unhandled charset: ESC ( %c\n",
					c);
			}
			term->esc = 0;
		} else if (term->esc & ESC_TEST) {
			if (c == '8') {	/* DEC screen alignment test. */
				struct coord p;

				for (p.x = 0; p.x < term->size.x; ++p.x)
					for (p.y = 0; p.y < term->size.y; ++p.y)
						tsetchar(term, 'E', p);
			}
			term->esc = 0;
		} else {
			switch (c) {
			case '[':
				term->esc |= ESC_CSI;
				break;
			case '#':
				term->esc |= ESC_TEST;
				break;
			case 'P':	/* DCS -- Device Control String */
			case '_':	/* APC -- Application Program Command */
			case '^':	/* PM -- Privacy Message */
			case ']':	/* OSC -- Operating System Command */
			case 'k':	/* old title set compatibility */
				strreset(&term->strescseq);
				term->strescseq.type = c;
				term->esc |= ESC_STR;
				break;
			case '(':	/* set primary charset G0 */
				term->esc |= ESC_ALTCHARSET;
				break;
			case ')':	/* set secondary charset G1 (IGNORED) */
			case '*':	/* set tertiary charset G2 (IGNORED) */
			case '+':	/* set quaternary charset G3 (IGNORED) */
				term->esc = 0;
				break;
			case 'D':	/* IND -- Linefeed */
				if (term->c.pos.y == term->bot)
					tscrollup(term, term->top, 1);
				else
					tmoverel(term, 0, 1);
				term->esc = 0;
				break;
			case 'E':	/* NEL -- Next line */
				tnewline(term, 1);	/* always go to first col */
				term->esc = 0;
				break;
			case 'H':	/* HTS -- Horizontal tab stop */
				term->tabs[term->c.pos.x] = 1;
				term->esc = 0;
				break;
			case 'M':	/* RI -- Reverse index */
				if (term->c.pos.y == term->top) {
					tscrolldown(term, term->top, 1);
				} else {
					tmoverel(term, 0, -1);
				}
				term->esc = 0;
				break;
			case 'Z':	/* DECID -- Identify Terminal */
				ttywrite(term, VT102ID, sizeof(VT102ID) - 1);
				term->esc = 0;
				break;
			case 'c':	/* RIS -- Reset to inital state */
				treset(term);
				term->esc = 0;
				if (term->settitle)
					term->settitle(term, NULL);
				break;
			case '=':	/* DECPAM -- Application keypad */
				term->appkeypad = 1;
				term->esc = 0;
				break;
			case '>':	/* DECPNM -- Normal keypad */
				term->appkeypad = 0;
				term->esc = 0;
				break;
			case '7':	/* DECSC -- Save Cursor */
				tcursor_save(term);
				term->esc = 0;
				break;
			case '8':	/* DECRC -- Restore Cursor */
				tcursor_load(term);
				term->esc = 0;
				break;
			case '\\':	/* ST -- Stop */
				term->esc = 0;
				break;
			default:
				fprintf(stderr,
					"erresc: unknown sequence ESC 0x%02X '%c'\n",
					(unsigned char) c,
					isprint(c) ? c : '.');
				term->esc = 0;
			}
		}
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	/*
	 * Display control codes only if we are in graphic mode
	 */
	if (control && !term->c.attr.gfx)
		return;

	if (term->sel.type != SEL_NONE &&
	    BETWEEN(term->c.pos.y, term->sel.p1.y, term->sel.p2.y))
		term->sel.type = SEL_NONE;

	if (term->wrap && term->c.wrapnext)
		tnewline(term, 1);	/* always go to first col */

	if (term->insert && term->c.pos.x + 1 < term->size.x)
		memmove(term_pos(term, term->c.pos) + 1,
			term_pos(term, term->c.pos),
			(term->size.x - term->c.pos.x - 1) * sizeof(struct st_glyph));

	tsetchar(term, c, term->c.pos);
	if (term->c.pos.x + 1 < term->size.x)
		tmoverel(term, 1, 0);
	else
		term->c.wrapnext = 1;
}

void term_echo(struct st_term *term, char *buf, int len)
{
	for (; len > 0; buf++, len--) {
		char c = *buf;

		if (c == '\033') {	/* escape */
			tputc(term, '^');
			tputc(term, '[');
		} else if (c < '\x20') {	/* control code */
			if (c != '\n' && c != '\r' && c != '\t') {
				c |= '\x40';
				tputc(term, '^');
			}
			tputc(term, c);
		} else {
			break;
		}
	}
	if (len) {
		unsigned ucs;

		FcUtf8ToUcs4((unsigned char *) buf, &ucs, len);
		tputc(term, ucs);
	}
}

void term_read(struct st_term *term)
{
	unsigned char *ptr;
	int ret;

	/* append read bytes to unprocessed bytes */
	if ((ret = read(term->cmdfd,
			term->cmdbuf + term->cmdbuflen,
			sizeof(term->cmdbuf) - term->cmdbuflen)) < 0)
		edie("Couldn't read from shell");

	if (term->logfd != -1 &&
	    xwrite(term->logfd, term->cmdbuf + term->cmdbuflen, ret) < 0) {
		fprintf(stderr, "Error writing in %s:%s\n",
			term->logfile, strerror(errno));
		close(term->logfd);
		term->logfd = -1;
	}

	/* process every complete utf8 char */
	term->cmdbuflen += ret;
	ptr = term->cmdbuf;

	while (term->cmdbuflen) {
		unsigned ucs;
		int charsize = FcUtf8ToUcs4(ptr, &ucs, term->cmdbuflen);
		if (charsize < 0) {
			charsize = 1;
			ucs = *ptr;
		}

		if (charsize > term->cmdbuflen)
			break;

		tputc(term, ucs);
		ptr += charsize;
		term->cmdbuflen -= charsize;
	}

	/* keep any uncomplete utf8 char for the next call */
	memmove(term->cmdbuf, ptr, term->cmdbuflen);
}

void term_mousereport(struct st_term *term, struct coord pos,
		      unsigned type, unsigned button, unsigned state)
{
	char buf[40];
	int len;

	/* from urxvt */
	if (type == MotionNotify) {
		if (!term->mousemotion ||
		    (pos.x == term->mousepos.x &&
		     pos.y == term->mousepos.y))
			return;

		button = term->mousebutton + 32;
		term->mousepos = pos;
	} else if (!term->mousesgr &&
		   (type == ButtonRelease ||
		    button == AnyButton)) {
		button = 3;
	} else {
		button -= Button1;
		if (button >= 3)
			button += 64 - 3;
		if (type == ButtonPress) {
			term->mousebutton = button;
			term->mousepos = pos;
		}
	}

	button += (state & ShiftMask ? 4 : 0) +
		(state & Mod4Mask ? 8 : 0) +
		(state & ControlMask ? 16 : 0);

	if (term->mousesgr)
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
			       button, pos.x + 1, pos.y + 1,
			       type == ButtonRelease ? 'm' : 'M');
	else if (pos.x < 223 && pos.y < 223)
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
			       32 + button, 32 + pos.x + 1, 32 + pos.y + 1);
	else
		return;

	ttywrite(term, buf, len);
}

/* Resize code */

static void ttyresize(struct st_term *term)
{
	struct winsize w;

	w.ws_row = term->size.y;
	w.ws_col = term->size.x;
	w.ws_xpixel = term->ttysize.x;
	w.ws_ypixel = term->ttysize.y;

	if (ioctl(term->cmdfd, TIOCSWINSZ, &w) < 0)
		perror("Couldn't set window size");
}

void term_resize(struct st_term *term, struct coord size)
{
	unsigned i, x;
	unsigned minrow = min(size.y, term->size.y);
	int slide = term->c.pos.y - size.y + 1;
	bool *bp;

	if (size.x < 1 || size.y < 1)
		return;

	/* free unneeded rows */
	i = 0;
	if (slide > 0) {
		/*
		 * slide screen to keep cursor where we expect it -
		 * tscrollup would work here, but we can optimize to
		 * memmove because we're freeing the earlier lines
		 */
		for ( /* i = 0 */ ; i < slide; i++) {
			free(term->line[i]);
			free(term->alt[i]);
		}
		memmove(term->line, term->line + slide,
			size.y * sizeof(struct st_glyph *));
		memmove(term->alt, term->alt + slide,
			size.y * sizeof(struct st_glyph *));
	}
	for (i += size.y; i < term->size.y; i++) {
		free(term->line[i]);
		free(term->alt[i]);
	}

	/* resize to new height */
	term->line = xrealloc(term->line, size.y * sizeof(struct st_glyph *));
	term->alt = xrealloc(term->alt, size.y * sizeof(struct st_glyph *));
	term->tabs = xrealloc(term->tabs, size.x * sizeof(*term->tabs));

	/* resize each row to new width, zero-pad if needed */
	for (i = 0; i < minrow; i++) {
		term->line[i] = xrealloc(term->line[i], size.x * sizeof(struct st_glyph));
		term->alt[i] = xrealloc(term->alt[i], size.x * sizeof(struct st_glyph));

		for (x = term->size.x; x < size.x; x++) {
			term->line[i][x] = term->c.attr;
			term->alt[i][x] = term->c.attr;
		}
	}

	/* allocate any new rows */
	for ( /* i == minrow */ ; i < size.y; i++) {
		term->line[i] = xcalloc(size.x, sizeof(struct st_glyph));
		term->alt[i] = xcalloc(size.x, sizeof(struct st_glyph));

		for (x = 0; x < size.x; x++) {
			term->line[i][x] = term->c.attr;
			term->alt[i][x] = term->c.attr;
		}
	}

	if (size.x > term->size.x) {
		bp = term->tabs + term->size.x;

		memset(bp, 0, sizeof(*term->tabs) * (size.x - term->size.x));
		while (--bp > term->tabs && !*bp)
			/* nothing */ ;
		for (bp += SPACES_PER_TAB; bp < term->tabs + size.x;
		     bp += SPACES_PER_TAB)
			*bp = 1;
	}
	/* update terminal size */
	term->size = size;
	/* reset scrolling region */
	tsetscroll(term, 0, size.y - 1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(term, term->c.pos);

	ttyresize(term);
}

/* Startup */

static pid_t pid;

void term_shutdown(struct st_term *term)
{
	kill(pid, SIGHUP);
}

static void execsh(unsigned long windowid, char *shell, char **cmd)
{
	char **args;
	char *envshell = getenv("SHELL");
	const struct passwd *pass = getpwuid(getuid());
	char buf[sizeof(long) * 8 + 1];

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");

	if (pass) {
		setenv("LOGNAME", pass->pw_name, 1);
		setenv("USER", pass->pw_name, 1);
		setenv("SHELL", pass->pw_shell, 0);
		setenv("HOME", pass->pw_dir, 0);
	}

	snprintf(buf, sizeof(buf), "%lu", windowid);
	setenv("WINDOWID", buf, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	DEFAULT(envshell, shell);
	setenv("TERM", TERMNAME, 1);
	args = cmd ? cmd : (char *[]) {
	envshell, "-i", NULL};
	execvp(args[0], args);
	exit(EXIT_FAILURE);
}

static void sigchld(int a)
{
	int stat = 0;

	if (waitpid(pid, &stat, 0) < 0)
		edie("Waiting for pid %hd failed");

	if (WIFEXITED(stat))
		exit(WEXITSTATUS(stat));
	else
		exit(EXIT_FAILURE);
}

static void term_ttyinit(struct st_term *term, unsigned long windowid,
			 char *shell, char **cmd)
{
	int m, s;
	struct winsize w = { term->size.y, term->size.x, 0, 0 };

	term->logfd = -1;

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, &w) < 0)
		edie("openpty failed");

	switch (pid = fork()) {
	case -1:
		edie("fork failed");
		break;
	case 0:
		setsid();	/* create a new process group */
		dup2(s, STDIN_FILENO);
		dup2(s, STDOUT_FILENO);
		dup2(s, STDERR_FILENO);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			edie("ioctl TIOCSCTTY failed");
		close(s);
		close(m);
		execsh(windowid, shell, cmd);
		break;
	default:
		close(s);
		term->cmdfd = m;
		signal(SIGCHLD, sigchld);
		if (term->logfile) {
			term->logfd = (!strcmp(term->logfile, "-")) ?
			    STDOUT_FILENO :
			    open(term->logfile, O_WRONLY | O_CREAT, 0666);
			if (term->logfd < 0) {
				fprintf(stderr, "Error opening %s:%s\n",
					term->logfile, strerror(errno));
			}
		}
	}
}

void term_init(struct st_term *term, int col, int row, char *shell,
	       char **cmd, const char *logfile, unsigned long windowid,
	       unsigned defaultfg, unsigned defaultbg, unsigned defaultcs)
{
	term->logfile	= logfile;
	term->defaultfg = defaultfg;
	term->defaultbg = defaultbg;
	term->defaultcs = defaultcs;

	/* set screen size */
	term->size.y = row;
	term->size.x = col;
	term->line = xcalloc(term->size.y, sizeof(struct st_glyph *));
	term->alt = xcalloc(term->size.y, sizeof(struct st_glyph *));
	term->tabs = xcalloc(term->size.x, sizeof(*term->tabs));

	for (row = 0; row < term->size.y; row++) {
		term->line[row] = xcalloc(term->size.x, sizeof(struct st_glyph));
		term->alt[row] = xcalloc(term->size.x, sizeof(struct st_glyph));
	}

	term->numlock = 1;
	/* setup screen */
	treset(term);
	term_ttyinit(term, windowid, shell, cmd);
}
