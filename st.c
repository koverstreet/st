/* See LICENSE for licence details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>

/* From linux kernel */
#define min(x, y) ({				\
	typeof(x) _min1 = (x);			\
	typeof(y) _min2 = (y);			\
	(void) (&_min1 == &_min2);		\
	_min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({				\
	typeof(x) _max1 = (x);			\
	typeof(y) _max2 = (y);			\
	(void) (&_max1 == &_max2);		\
	_max1 > _max2 ? _max1 : _max2; })

#define clamp(val, min, max) ({			\
	typeof(val) __val = (val);		\
	typeof(min) __min = (min);		\
	typeof(max) __max = (max);		\
	(void) (&__val == &__min);		\
	(void) (&__val == &__max);		\
	__val = __val < __min ? __min: __val;	\
	__val > __max ? __max: __val; })

#define clamp_t(type, val, min, max) ({		\
	type __val = (val);			\
	type __min = (min);			\
	type __max = (max);			\
	__val = __val < __min ? __min: __val;	\
	__val > __max ? __max: __val; })

#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#if   defined(__linux)
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#endif

#define USAGE \
	"st " VERSION " (c) 2010-2013 st engineers\n" \
	"usage: st [-v] [-c class] [-f font] [-g geometry] [-o file]" \
	" [-t title] [-w windowid] [-e command ...]\n"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* Arbitrary sizes */
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ
#define DRAW_BUF_SIZ  20*1024
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

#define REDRAW_TIMEOUT (80*1000)	/* 80 ms */

/* macros */
#define SERRNO strerror(errno)
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define TIMEDIFF(t1, t2) ((t1.tv_sec-t2.tv_sec)*1000 + (t1.tv_usec-t2.tv_usec)/1000)

#define VT102ID "\033[?6c"

enum escape_state {
	ESC_START = 1,
	ESC_CSI = 2,
	ESC_STR = 4,		/* DSC, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END = 16,	/* a final string was encountered */
	ESC_TEST = 32,		/* Enter in test mode */
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

struct st_glyph {
	char		c[UTF_SIZ];	/* character code */
	union {
	unsigned	cmp;
	struct {
		unsigned	fg:12;		/* foreground  */
		unsigned	bg:12;		/* background  */
		unsigned	reverse:1;
		unsigned	underline:1;
		unsigned	bold:1;
		unsigned	gfx:1;
		unsigned	italic:1;
		unsigned	blink:1;
		unsigned	set:1;
	};
	};
};

struct coord {
	unsigned	x, y;
};

#define ORIGIN	(struct coord) {0, 0}

struct tcursor {
	struct st_glyph	attr;		/* current char attributes */
	struct coord	pos;
	unsigned	wrapnext:1;
	unsigned	origin:1;
};

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode>] */
struct csi_escape {
	char		buf[ESC_BUF_SIZ]; /* raw string */
	int		len;		/* raw string length */
	char		priv;
	int		arg[ESC_ARG_SIZ];
	int		narg;		/* nb of args */
	char		mode;
};

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
struct str_escape {
	char		type;		/* ESC type ... */
	char		buf[STR_BUF_SIZ]; /* raw string */
	int		len;		/* raw string length */
	char		*args[STR_ARG_SIZ];
	int		narg;		/* nb of args */
};

/* TODO: use better name for vars... */
struct st_selection {
	int		mode;
	int		type;
	int		bx, by;
	int		ex, ey;
	struct {
		int	x, y;
	} b, e;
	char		*clip;
	Atom		xtarget;
	bool		alt;
	struct timeval	tclick1;
	struct timeval	tclick2;
};

/* Internal representation of the screen */
struct st_term {
	int		cmdfd;
	unsigned char	cmdbuf[BUFSIZ];
	unsigned	cmdbuflen;

	struct coord	size;
	struct st_glyph	**line;	/* screen */
	struct st_glyph	**alt;	/* alternate screen */
	bool		*dirty;	/* dirtyness of lines */
	bool		*tabs;

	struct tcursor	c;	/* cursor */
	struct tcursor	saved;
	struct coord	oldcursor;
	struct st_selection sel;
	unsigned	top;	/* top    scroll limit */
	unsigned	bot;	/* bottom scroll limit */

	unsigned	wrap:1;
	unsigned	insert:1;
	unsigned	appkeypad:1;
	unsigned	altscreen:1;
	unsigned	crlf:1;
	unsigned	mousebtn:1;
	unsigned	mousemotion:1;
	unsigned	reverse:1;
	unsigned	kbdlock:1;
	unsigned	hide:1;
	unsigned	echo:1;
	unsigned	appcursor:1;
	unsigned	mousesgr:1;
	unsigned	numlock:1;

	int		esc;	/* escape state flags */
	struct csi_escape csiescseq;
	struct str_escape strescseq;
};

/* Font structure */
struct st_font {
	int		height;
	int		width;
	int		ascent;
	int		descent;
	short		lbearing;
	short		rbearing;
	XftFont		*match;
	FcFontSet	*set;
	FcPattern	*pattern;
};

struct st_key {
	KeySym		k;
	unsigned	mask;
	char		s[ESC_BUF_SIZ];
	/* three valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char	appkey;		/* application keypad */
	signed char	appcursor;	/* application cursor */
	signed char	crlf;		/* crlf mode          */
};

union st_arg {
	int		i;
	unsigned int	ui;
	float		f;
	const void	*v;
};

struct st_window;

struct st_shortcut {
	unsigned int	mod;
	KeySym		keysym;
	void (*func) (struct st_window *, const union st_arg *);
	const union st_arg arg;
};

/* function definitions used in config.h */
static void clippaste(struct st_window *, const union st_arg *);
static void selpaste(struct st_window *, const union st_arg *);
static void numlock(struct st_window *, const union st_arg *);
static void xzoom(struct st_window *, const union st_arg *);

/* Config.h for applying patches and the configuration. */
#include "config.h"

struct st_window {
	struct st_term	term;

	/* Graphic info */
	XftColor	col[ARRAY_SIZE(colorname) < 256 ? 256 : ARRAY_SIZE(colorname)];
	GC		gc;
	Display		*dpy;
	Colormap	cmap;
	Window		win;
	Drawable	buf;
	Atom		xembed;
	Atom		wmdeletewin;
	XIM		xim;
	XIC		xic;
	XftDraw		*draw;
	Visual		*vis;
	char		*default_title;
	char		*class;
	char		*embed;

	struct st_font	font, bfont, ifont, ibfont;

	int		scr;
	bool		isfixed;	/* is fixed geometry? */
	int		fx, fy;		/* fixed geometry */
	struct coord	ttysize;
	struct coord	winsize;
	struct coord	fixedsize;	/* kill? */
	struct coord	charsize;
	struct coord	mousepos;
	unsigned	mousebutton;
	unsigned	visible:1;
	unsigned	redraw:1;
	unsigned	focused:1;
};

/* Globals */
static pid_t pid;
static int iofd = -1;
static char **opt_cmd = NULL;
static char *opt_io = NULL;
static char *opt_font = NULL;

/* Random utility code */

static void die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static void execsh(unsigned long windowid)
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
	setenv("TERM", termname, 1);
	args = opt_cmd ? opt_cmd : (char *[]) {
	envshell, "-i", NULL};
	execvp(args[0], args);
	exit(EXIT_FAILURE);
}

static void sigchld(int a)
{
	int stat = 0;

	if (waitpid(pid, &stat, 0) < 0)
		die("Waiting for pid %hd failed: %s\n", pid, SERRNO);

	if (WIFEXITED(stat))
		exit(WEXITSTATUS(stat));
	else
		exit(EXIT_FAILURE);
}

static ssize_t xwrite(int fd, void *s, size_t len)
{
	size_t aux = len;

	while (len > 0) {
		ssize_t r = write(fd, s, len);
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}
	return aux;
}

static void *xmalloc(size_t len)
{
	void *p = malloc(len);

	if (!p)
		die("Out of memory\n");

	return p;
}

static void *xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("Out of memory\n");

	return p;
}

static void *xcalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);

	if (!p)
		die("Out of memory\n");

	return p;
}

static int utf8size(char *s)
{
	unsigned ucs;
	return FcUtf8ToUcs4((unsigned char *) s, &ucs, UTF_SIZ);
}

static unsigned short sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

/* X utility code */

static bool match(unsigned mask, uint state)
{
	state &= ~(ignoremod);

	if (mask == XK_NO_MOD && state)
		return false;
	if (mask != XK_ANY_MOD && mask != XK_NO_MOD && !state)
		return false;
	if ((state & mask) != state)
		return false;
	return true;
}

static int xsetcolorname(struct st_window *xw,
			 int x, const char *name)
{
	XRenderColor color = {.alpha = 0xffff };
	XftColor colour;
	if (x < 0 || x > ARRAY_SIZE(colorname))
		return -1;
	if (!name) {
		if (16 <= x && x < 16 + 216) {
			int r = (x - 16) / 36, g = ((x - 16) % 36) / 6, b =
			    (x - 16) % 6;
			color.red = sixd_to_16bit(r);
			color.green = sixd_to_16bit(g);
			color.blue = sixd_to_16bit(b);
			if (!XftColorAllocValue(xw->dpy, xw->vis,
						xw->cmap, &color, &colour))
				return 0;	/* something went wrong */
			xw->col[x] = colour;
			return 1;
		} else if (16 + 216 <= x && x < 256) {
			color.red = color.green = color.blue =
			    0x0808 + 0x0a0a * (x - (16 + 216));
			if (!XftColorAllocValue(xw->dpy, xw->vis,
						xw->cmap, &color, &colour))
				return 0;	/* something went wrong */
			xw->col[x] = colour;
			return 1;
		} else {
			name = colorname[x];
		}
	}
	if (!XftColorAllocName(xw->dpy, xw->vis, xw->cmap, name, &colour))
		return 0;
	xw->col[x] = colour;
	return 1;
}

static void xsettitle(struct st_window *xw, char *p)
{
	XTextProperty prop;

	Xutf8TextListToTextProperty(xw->dpy, &p, 1, XUTF8StringStyle,
				    &prop);
	XSetWMName(xw->dpy, xw->win, &prop);
}

static void xresettitle(struct st_window *xw)
{
	xsettitle(xw, xw->default_title);
}

static void xseturgency(struct st_window *xw, int add)
{
	XWMHints *h = XGetWMHints(xw->dpy, xw->win);

	h->flags =
	    add ? (h->flags | XUrgencyHint) : (h->flags & ~XUrgencyHint);
	XSetWMHints(xw->dpy, xw->win, h);
	XFree(h);
}

static void xsetsel(struct st_window *xw)
{
	Atom clipboard;

	XSetSelectionOwner(xw->dpy, XA_PRIMARY, xw->win, CurrentTime);

	clipboard = XInternAtom(xw->dpy, "CLIPBOARD", 0);
	XSetSelectionOwner(xw->dpy, clipboard, xw->win, CurrentTime);
}

static void ttywrite(struct st_term *term, const char *s, size_t n)
{
	if (write(term->cmdfd, s, n) == -1)
		die("write error on tty: %s\n", SERRNO);
}

/* ? */

static void tsetdirt(struct st_term *term, unsigned top, unsigned bot)
{
	bot = min(bot, term->size.y - 1);

	for (unsigned i = top; i <= bot; i++)
		term->dirty[i] = 1;
}

static void tfulldirt(struct st_term *term)
{
	tsetdirt(term, 0, term->size.y - 1);
}

/* Selection code */

static bool selected(struct st_selection *sel, int x, int y)
{
	int bx, ex;

	if (sel->ey == y && sel->by == y) {
		bx = min(sel->bx, sel->ex);
		ex = max(sel->bx, sel->ex);
		return BETWEEN(x, bx, ex);
	}

	return ((sel->b.y < y && y < sel->e.y)
		|| (y == sel->e.y && x <= sel->e.x))
	    || (y == sel->b.y && x >= sel->b.x
		&& (x <= sel->e.x || sel->b.y != sel->e.y));

	switch (sel->type) {
	case SEL_REGULAR:
		return ((sel->b.y < y && y < sel->e.y)
			|| (y == sel->e.y && x <= sel->e.x))
		    || (y == sel->b.y && x >= sel->b.x
			&& (x <= sel->e.x || sel->b.y != sel->e.y));
	case SEL_RECTANGULAR:
		return ((sel->b.y <= y && y <= sel->e.y)
			&& (sel->b.x <= x && x <= sel->e.x));
	};
}

static void selcopy(struct st_window *xw)
{
	struct st_term *term = &xw->term;
	char *str, *ptr, *p;
	int x, y, bufsize, is_selected = 0, size;
	struct st_glyph *gp, *last;

	if (term->sel.bx == -1) {
		str = NULL;
	} else {
		bufsize = (term->size.y + 1) *
			(term->sel.e.y - term->sel.b.y + 1) * UTF_SIZ;
		ptr = str = xmalloc(bufsize);

		/* append every set & selected glyph to the selection */
		for (y = term->sel.b.y; y < term->sel.e.y + 1; y++) {
			is_selected = 0;
			gp = &term->line[y][0];
			last = gp + term->size.y;

			while (--last >= gp && !last->set)
				/* nothing */ ;

			for (x = 0; gp <= last; x++, ++gp) {
				if (!selected(&term->sel, x, y)) {
					continue;
				} else {
					is_selected = 1;
				}

				p = gp->set ? gp->c : " ";
				size = utf8size(p);
				memcpy(ptr, p, size);
				ptr += size;
			}
			/* \n at the end of every selected line except for the last one */
			if (is_selected && y < term->sel.e.y)
				*ptr++ = '\r';
		}
		*ptr = 0;
	}

	free(term->sel.clip);
	term->sel.clip = str;
	xsetsel(xw);
}

static void selnotify(struct st_window *xw, XEvent *e)
{
	unsigned long nitems, ofs, rem;
	int format;
	unsigned char *data;
	Atom type;

	ofs = 0;
	do {
		if (XGetWindowProperty
		    (xw->dpy, xw->win, XA_PRIMARY, ofs, BUFSIZ / 4, False,
		     AnyPropertyType, &type, &format, &nitems, &rem,
		     &data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}
		ttywrite(&xw->term, (const char *) data, nitems * format / 8);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while (rem > 0);
}

static void selpaste(struct st_window *xw, const union st_arg *dummy)
{
	XConvertSelection(xw->dpy, XA_PRIMARY, xw->term.sel.xtarget,
			  XA_PRIMARY, xw->win, CurrentTime);
}

static void clippaste(struct st_window *xw, const union st_arg *dummy)
{
	Atom clipboard;

	clipboard = XInternAtom(xw->dpy, "CLIPBOARD", 0);
	XConvertSelection(xw->dpy, clipboard, xw->term.sel.xtarget,
			  XA_PRIMARY, xw->win, CurrentTime);
}

static void selclear(struct st_window *xw, XEvent *e)
{
	struct st_selection *sel = &xw->term.sel;

	if (sel->bx == -1)
		return;
	sel->bx = -1;
	tsetdirt(&xw->term, sel->b.y, sel->e.y);
}

static void selrequest(struct st_window *xw, XEvent *e)
{
	struct st_selection *sel = &xw->term.sel;
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xw->dpy, "TARGETS", 0);
	if (xsre->target == xa_targets) {
		/* respond with the supported type */
		string = sel->xtarget;
		XChangeProperty(xsre->display, xsre->requestor,
				xsre->property, XA_ATOM, 32,
				PropModeReplace, (unsigned char *) & string, 1);
		xev.property = xsre->property;
	} else if (xsre->target == sel->xtarget && sel->clip != NULL) {
		XChangeProperty(xsre->display, xsre->requestor,
				xsre->property, xsre->target, 8,
				PropModeReplace, (unsigned char *) sel->clip,
				strlen(sel->clip));
		xev.property = xsre->property;
	}

	/* all done, send a notification to the listener */
	if (!XSendEvent
	    (xsre->display, xsre->requestor, True, 0, (XEvent *) & xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

static void selscroll(struct st_term *term, int orig, int n)
{
	if (term->sel.bx == -1)
		return;

	if (BETWEEN(term->sel.by, orig, term->bot)
	    || BETWEEN(term->sel.ey, orig, term->bot)) {
		if ((term->sel.by += n) > term->bot ||
		    (term->sel.ey += n) < term->top) {
			term->sel.bx = -1;
			return;
		}

		switch (term->sel.type) {
		case SEL_REGULAR:
			if (term->sel.by < term->top) {
				term->sel.by = term->top;
				term->sel.bx = 0;
			}
			if (term->sel.ey > term->bot) {
				term->sel.ey = term->bot;
				term->sel.ex = term->size.y;
			}
			break;
		case SEL_RECTANGULAR:
			if (term->sel.by < term->top)
				term->sel.by = term->top;
			if (term->sel.ey > term->bot)
				term->sel.ey = term->bot;
			break;
		};
		term->sel.b.y = term->sel.by, term->sel.b.x = term->sel.bx;
		term->sel.e.y = term->sel.ey, term->sel.e.x = term->sel.ex;
	}
}

/* Screen drawing code */

static char *usedfont = NULL;
static int usedfontsize = 0;

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

struct st_fontcache {
	XftFont *font;
	long c;
	int flags;
};

/*
 * Fontcache is a ring buffer, with frccur as current position and frclen as
 * the current length of used elements.
 */

static struct st_fontcache frc[1024];
static int frccur = -1, frclen = 0;

static void xtermclear(struct st_window *xw,
		       int col1, int row1, int col2, int row2)
{
	XftDrawRect(xw->draw,
		    &xw->col[xw->term.reverse ? defaultfg : defaultbg],
		    borderpx + col1 * xw->charsize.x,
		    borderpx + row1 * xw->charsize.y,
		    (col2 - col1 + 1) * xw->charsize.x,
		    (row2 - row1 + 1) * xw->charsize.y);
}

/*
 * Absolute coordinates.
 */
static void xclear(struct st_window *xw,
		   int x1, int y1, int x2, int y2)
{
	XftDrawRect(xw->draw,
		    &xw->col[xw->term.reverse ? defaultfg : defaultbg],
		    x1, y1, x2 - x1, y2 - y1);
}

static void xdraws(struct st_window *xw,
		   char *s, struct st_glyph base,
		   struct coord pos,
		   int charlen, int bytelen)
{
	int winx = borderpx + pos.x * xw->charsize.x;
	int winy = borderpx + pos.y * xw->charsize.y;
	int width = charlen * xw->charsize.x;
	int xp, i, frp, frcflags;
	int u8fl, u8fblen, u8cblen, doesexist;
	char *u8c, *u8fs;
	unsigned u8char;
	struct st_font *font = &xw->font;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	XftColor *fg = &xw->col[base.fg], *bg = &xw->col[base.bg],
	    revfg, revbg;
	XRenderColor colfg, colbg;

	frcflags = FRC_NORMAL;

	if (base.bold) {
		if (BETWEEN(base.fg, 0, 7)) {
			/* basic system colors */
			fg = &xw->col[base.fg + 8];
		} else if (BETWEEN(base.fg, 16, 195)) {
			/* 256 colors */
			fg = &xw->col[base.fg + 36];
		} else if (BETWEEN(base.fg, 232, 251)) {
			/* greyscale */
			fg = &xw->col[base.fg + 4];
		}
		/*
		 * Those ranges will not be brightened:
		 *      8 - 15 – bright system colors
		 *      196 - 231 – highest 256 color cube
		 *      252 - 255 – brightest colors in greyscale
		 */
		font = &xw->bfont;
		frcflags = FRC_BOLD;
	}

	if (base.italic) {
		font = &xw->ifont;
		frcflags = FRC_ITALIC;
	}
	if (base.italic && base.bold) {
		font = &xw->ibfont;
		frcflags = FRC_ITALICBOLD;
	}

	if (xw->term.reverse) {
		if (fg == &xw->col[defaultfg]) {
			fg = &xw->col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw->dpy, xw->vis, xw->cmap, &colfg,
					   &revfg);
			fg = &revfg;
		}

		if (bg == &xw->col[defaultbg]) {
			bg = &xw->col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw->dpy, xw->vis, xw->cmap, &colbg,
					   &revbg);
			bg = &revbg;
		}
	}

	if (base.reverse)
		swap(bg, fg);

	/* Intelligent cleaning up of the borders. */
	if (pos.x == 0)
		xclear(xw, 0, (pos.y == 0) ? 0 : winy, borderpx,
		       winy + xw->charsize.y +
		       ((pos.y >= xw->term.size.y - 1) ? xw->winsize.y : 0));

	if (pos.x + charlen >= xw->term.size.x)
		xclear(xw, winx + width, (pos.y == 0)
		       ? 0 : winy, xw->winsize.x,
		       ((pos.y >= xw->term.size.y - 1)
			? xw->winsize.y : (winy + xw->charsize.y)));

	if (pos.y == 0)
		xclear(xw, winx, 0, winx + width, borderpx);
	if (pos.y == xw->term.size.y - 1)
		xclear(xw, winx, winy + xw->charsize.y, winx + width, xw->winsize.y);

	/* Clean up the region we want to draw to. */
	XftDrawRect(xw->draw, bg, winx, winy, width, xw->charsize.y);

	fcsets[0] = font->set;
	for (xp = winx; bytelen > 0;) {
		/*
		 * Search for the range in the to be printed string of glyphs
		 * that are in the main font. Then print that range. If
		 * some glyph is found that is not in the font, do the
		 * fallback dance.
		 */
		u8fs = s;
		u8fblen = 0;
		u8fl = 0;
		for (;;) {
			u8c = s;
			u8cblen = FcUtf8ToUcs4((unsigned char *) s,
					       &u8char, UTF_SIZ);
			s += u8cblen;
			bytelen -= u8cblen;

			doesexist =
			    XftCharIndex(xw->dpy, font->match, u8char);
			if (!doesexist || bytelen <= 0) {
				if (bytelen <= 0) {
					if (doesexist) {
						u8fl++;
						u8fblen += u8cblen;
					}
				}

				if (u8fl > 0) {
					XftDrawStringUtf8(xw->draw, fg,
							  font->match, xp,
							  winy +
							  font->ascent,
							  (FcChar8 *) u8fs,
							  u8fblen);
					xp += font->width * u8fl;
				}
				break;
			}

			u8fl++;
			u8fblen += u8cblen;
		}
		if (doesexist)
			break;

		frp = frccur;
		/* Search the font cache. */
		for (i = 0; i < frclen; i++, frp--) {
			if (frp <= 0)
				frp = ARRAY_SIZE(frc) - 1;

			if (frc[frp].c == u8char
			    && frc[frp].flags == frcflags) {
				break;
			}
		}

		/* Nothing was found. */
		if (i >= frclen) {
			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, u8char);
			FcPatternAddCharSet(fcpattern, FC_CHARSET,
					    fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE, FcTrue);

			FcConfigSubstitute(0, fcpattern, FcMatchPattern);
			FcDefaultSubstitute(fcpattern);

			fontpattern = FcFontSetMatch(0, fcsets,
						     FcTrue, fcpattern,
						     &fcres);

			/*
			 * Overwrite or create the new cache entry
			 * entry.
			 */
			frccur++;
			frclen++;
			if (frccur >= ARRAY_SIZE(frc))
				frccur = 0;
			if (frclen > ARRAY_SIZE(frc)) {
				frclen = ARRAY_SIZE(frc);
				XftFontClose(xw->dpy, frc[frccur].font);
			}

			frc[frccur].font = XftFontOpenPattern(xw->dpy,
							      fontpattern);
			frc[frccur].c = u8char;
			frc[frccur].flags = frcflags;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);

			frp = frccur;
		}

		XftDrawStringUtf8(xw->draw, fg, frc[frp].font,
				  xp, winy + frc[frp].font->ascent,
				  (FcChar8 *) u8c, u8cblen);

		xp += font->width;
	}

	/*
	   XftDrawStringUtf8(xw->draw, fg, font->set, winx,
	   winy + font->ascent, (FcChar8 *)s, bytelen);
	 */

	if (base.underline)
		XftDrawRect(xw->draw, fg, winx, winy + font->ascent + 1,
			    width, 1);
}

struct st_glyph *term_pos(struct st_term *term, struct coord pos)
{
	return &term->line[pos.y][pos.x];
}

static void xdrawcursor(struct st_window *xw)
{
	int sl;
	struct st_glyph g, *p, *old;

	g.c[0] = ' ';
	g.cmp = 0;
	g.bg = defaultcs;
	g.fg = defaultbg;

	xw->term.oldcursor.x = min(xw->term.oldcursor.x, xw->term.size.x - 1);
	xw->term.oldcursor.y = min(xw->term.oldcursor.y, xw->term.size.y - 1);

	p = term_pos(&xw->term, xw->term.c.pos);

	if (p->set)
		memcpy(g.c, p->c, UTF_SIZ);

	/* remove the old cursor */
	old = term_pos(&xw->term, xw->term.oldcursor);
	if (old->set) {
		sl = utf8size(old->c);
		xdraws(xw, old->c, *old, xw->term.oldcursor, 1, sl);
	} else {
		xtermclear(xw,
			   xw->term.oldcursor.x, xw->term.oldcursor.y,
			   xw->term.oldcursor.x, xw->term.oldcursor.y);
	}

	/* draw the new one */
	if (!xw->term.hide) {
		if (!xw->focused)
			g.bg = defaultucs;

		g.reverse = xw->term.reverse;
		if (g.reverse) {
			unsigned t = g.fg;
			g.fg = g.bg;
			g.bg = t;
		}

		sl = utf8size(g.c);
		xdraws(xw, g.c, g, xw->term.c.pos, 1, sl);
		xw->term.oldcursor = xw->term.c.pos;
	}
}

static void drawregion(struct st_window *xw,
		       int x1, int y1, int x2, int y2)
{
	int ic, ib, x, y, ox, sl;
	struct st_glyph base, new;
	char buf[DRAW_BUF_SIZ];
	bool ena_sel = xw->term.sel.bx != -1;

	if (xw->term.sel.alt != xw->term.altscreen)
		ena_sel = 0;

	if (!xw->visible)
		return;

	for (y = y1; y < y2; y++) {
		if (!xw->term.dirty[y])
			continue;

		xtermclear(xw, 0, y, xw->term.size.x, y);
		xw->term.dirty[y] = 0;
		base = xw->term.line[y][0];
		ic = ib = ox = 0;
		for (x = x1; x < x2; x++) {
			new = xw->term.line[y][x];
			if (ena_sel && *(new.c) && selected(&xw->term.sel, x, y))
				new.reverse ^= 1;
			if (ib > 0 && new.cmp != base.cmp) {
				xdraws(xw, buf, base, (struct coord) {ox, y},
				       ic, ib);
				ic = ib = 0;
			}
			if (new.set) {
				if (ib == 0) {
					ox = x;
					base = new;
				}

				sl = utf8size(new.c);
				memcpy(buf + ib, new.c, sl);
				ib += sl;
				++ic;
			}
		}
		if (ib > 0)
			xdraws(xw, buf, base, (struct coord) {ox, y}, ic, ib);
	}
	xdrawcursor(xw);
}

static void draw(struct st_window *xw)
{
	drawregion(xw, 0, 0, xw->term.size.x, xw->term.size.y);
	XCopyArea(xw->dpy, xw->buf, xw->win, xw->gc,
		  0, 0, xw->winsize.x, xw->winsize.y, 0, 0);
	XSetForeground(xw->dpy, xw->gc,
		       xw->col[xw->term.reverse ? defaultfg : defaultbg].pixel);
}

static void redraw(struct st_window *xw, int timeout)
{
	struct timespec tv = { 0, timeout * 1000 };

	tfulldirt(&xw->term);
	draw(xw);

	if (timeout > 0) {
		nanosleep(&tv, NULL); /* XXX !?!?!? */
		XSync(xw->dpy, False);	/* necessary for a good tput flash */
	}
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

static void __tclearregion(struct st_term *term, struct coord p1,
			 struct coord p2, int bce)
{
	struct coord p;

	for (p.y = p1.y; p.y < p2.y; p.y++) {
		term->dirty[p.y] = 1;

		for (p.x = p1.x; p.x < p2.x; p.x++) {
			struct st_glyph *g = term_pos(term, p);

			g->set = bce;

			if (g->set) {
				*g = term->c.attr;

				memcpy(g->c, " ", 2);
				g->set = 1;
			}
		}
	}
}

static void tclearregion(struct st_term *term, struct coord p1,
			 struct coord p2, int bce)
{
	struct coord p;

	if (p1.x > p2.x)
		swap(p1.x, p2.x);
	if (p1.y > p2.y)
		swap(p1.y, p2.y);

	p1.x = min(p1.x, term->size.x - 1);
	p2.x = min(p2.x, term->size.x - 1);
	p1.y = min(p1.y, term->size.y - 1);
	p2.y = min(p2.y, term->size.y - 1);

	for (p.y = p1.y; p.y <= p2.y; p.y++) {
		term->dirty[p.y] = 1;

		for (p.x = p1.x; p.x <= p2.x; p.x++) {
			struct st_glyph *g = term_pos(term, p);

			g->set = bce;

			if (g->set) {
				*g = term->c.attr;

				memcpy(g->c, " ", 2);
				g->set = 1;
			}
		}
	}
}

static void tscrolldown(struct st_term *term, int orig, int n)
{
	int i;

	n = clamp_t(int, n, 0, term->bot - orig + 1);

	tclearregion(term,
		     (struct coord) {0, term->bot - n + 1},
		     (struct coord) {term->size.x - 1, term->bot}, 0);

	for (i = term->bot; i >= orig + n; i--) {
		swap(term->line[i], term->line[i - n]);

		term->dirty[i] = 1;
		term->dirty[i - n] = 1;
	}

	selscroll(term, orig, n);
}

static void tscrollup(struct st_term *term, int orig, int n)
{
	int i;

	n = clamp_t(int, n, 0, term->bot - orig + 1);

	tclearregion(term,
		     (struct coord) {0, orig},
		     (struct coord) {term->size.x - 1, orig + n - 1}, 0);

	/* XXX: optimize? */
	for (i = orig; i <= term->bot - n; i++) {
		swap(term->line[i], term->line[i + n]);

		term->dirty[i] = 1;
		term->dirty[i + n] = 1;
	}

	selscroll(term, orig, -n);
}

static void tmovex(struct st_term *term, unsigned x)
{
	term->c.wrapnext = 0;
	term->c.pos.x = min(x, term->size.x - 1);
}

static void tmovey(struct st_term *term, unsigned y)
{
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
	unsigned i;

	memset(&term->c, 0, sizeof(term->c));
	term->c.attr.cmp = 0;
	term->c.attr.fg = defaultcs;
	term->c.attr.bg = defaultbg;

	memset(term->tabs, 0, term->size.x * sizeof(*term->tabs));
	for (i = tabspaces; i < term->size.x; i += tabspaces)
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

	__tclearregion(term, ORIGIN, term->size, 0);
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

static void tsetchar(struct st_term *term, const char *c, struct coord pos)
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
		if (c[0] >= 0x41 && c[0] <= 0x7e && vt100_0[c[0] - 0x41])
			c = vt100_0[c[0] - 0x41];

	term->dirty[pos.y] = 1;
	*g = term->c.attr;
	memcpy(g->c, c, UTF_SIZ);
	g->set = 1;
}

static void tdeletechar(struct st_term *term, int n)
{
	unsigned size;
	struct coord src = term->c.pos, dst = term->c.pos;
	struct coord start = term->c.pos, end = term->c.pos;

	src.x += n;
	size = term->size.x - src.x;

	end.x = term->size.x - 1;

	if (src.x < term->size.x) {
		memmove(term_pos(term, dst),
			term_pos(term, src),
			size * sizeof(struct st_glyph));

		start.x = term->size.x - n;
	}

	tclearregion(term, start, end, 0);
}

static void tinsertblank(struct st_term *term, int n)
{
	unsigned size;
	struct coord src = term->c.pos, dst = term->c.pos;
	struct coord start = term->c.pos, end = term->c.pos;

	dst.x += n;
	size = term->size.x - dst.x;

	end.x = term->size.x - 1;

	if (dst.x < term->size.x) {
		memmove(term_pos(term, dst),
			term_pos(term, src),
			size * sizeof(struct st_glyph));

		end.x = dst.x - 1;
	}

	tclearregion(term, start, end, 0);
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
			g->fg		= defaultfg;
			g->bg		= defaultbg;
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
			term->c.attr.fg = defaultfg;
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
			term->c.attr.bg = defaultbg;
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
	term->altscreen ^= 1;
	tfulldirt(term);
}

static void tsetmode(struct st_window *xw,
		     bool priv, bool set, int *args, int narg)
{
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))
	struct st_term *term = &xw->term;
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
					redraw(xw, REDRAW_TIMEOUT);
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
						__tclearregion(term, ORIGIN,
							       term->size, 0);
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

static void csihandle(struct st_window *xw)
{
	struct st_term *term = &xw->term;
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
		term->sel.bx = -1;
		switch (csi->arg[0]) {
		case 0:	/* below */
			__tclearregion(term, term->c.pos, term->size, 1);
			if (term->c.pos.y < term->size.y - 1)
				__tclearregion(term, (struct coord)
					       {0, term->c.pos.y + 1},
					       term->size, 1);
			break;
		case 1:	/* above */
			if (term->c.pos.y > 1)
				__tclearregion(term, ORIGIN, term->size, 1);
			tclearregion(term, (struct coord) {0, term->c.pos.y},
				     term->c.pos, 1);
			break;
		case 2:	/* all */
			__tclearregion(term, ORIGIN, term->size, 1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K':		/* EL -- Clear line */
		switch (csi->arg[0]) {
		case 0:	/* right */
			tclearregion(term, term->c.pos, (struct coord)
				     {term->size.x - 1, term->c.pos.y}, 1);
			break;
		case 1:	/* left */
			tclearregion(term, (struct coord)
				     {0, term->c.pos.y}, term->c.pos, 1);
			break;
		case 2:	/* all */
			tclearregion(term, (struct coord) {0, term->c.pos.y},
				     (struct coord)
				     {term->size.x - 1, term->c.pos.y}, 1);
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
		tsetmode(xw, csi->priv, 0, csi->arg, csi->narg);
		break;
	case 'M':		/* DL -- Delete <n> lines */
		DEFAULT(csi->arg[0], 1);
		tdeleteline(term, csi->arg[0]);
		break;
	case 'X':		/* ECH -- Erase <n> char */
		DEFAULT(csi->arg[0], 1);
		tclearregion(term, term->c.pos, (struct coord)
			     {term->c.pos.x + csi->arg[0] - 1, term->c.pos.y}, 1);
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
		tsetmode(xw, csi->priv, 1, csi->arg, csi->narg);
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

static void strhandle(struct st_window *xw)
{
	struct str_escape *esc = &xw->term.strescseq;
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
			if (narg > 1)
				xsettitle(xw, esc->args[1]);
			break;
		case 4:	/* color set */
			if (narg < 3)
				break;
			p = esc->args[2];
			/* fall through */
		case 104:	/* color reset, here p = NULL */
			j = (narg > 1) ? atoi(esc->args[1]) : -1;
			if (!xsetcolorname(xw, j, p)) {
				fprintf(stderr,
					"erresc: invalid color %s\n", p);
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				redraw(xw, 0);
			}
			break;
		default:
			fprintf(stderr, "erresc: unknown str ");
			strdump(esc);
			break;
		}
		break;
	case 'k':		/* old title set compatibility */
		xsettitle(xw, esc->args[0]);
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

/* more random input code */

static void tputc(struct st_window *xw,
		  char *c, int len)
{
	unsigned char ascii = *c;
	bool control = ascii < '\x20' || ascii == 0177;
	struct st_term *term = &xw->term;

	/*
	 * STR sequences must be checked before anything else
	 * because it can use some control codes as part of the sequence.
	 */
	if (term->esc & ESC_STR) {
		switch (ascii) {
		case '\033':
			term->esc = ESC_START | ESC_STR_END;
			break;
		case '\a':	/* backwards compatibility to xterm */
			term->esc = 0;
			strhandle(xw);
			break;
		default:
			if (term->strescseq.len + len <
			    sizeof(term->strescseq.buf) - 1) {
				memmove(&term->strescseq.buf[term->strescseq.len],
					c, len);
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
		switch (ascii) {
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
			if (!xw->focused)
				xseturgency(xw, 1);
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
			term->csiescseq.buf[term->csiescseq.len++] = ascii;
			if (BETWEEN(ascii, 0x40, 0x7E)
			    || term->csiescseq.len >= sizeof(term->csiescseq.buf) - 1) {
				term->esc = 0;
				csiparse(&term->csiescseq);
				csihandle(xw);
			}
		} else if (term->esc & ESC_STR_END) {
			term->esc = 0;
			if (ascii == '\\')
				strhandle(xw);
		} else if (term->esc & ESC_ALTCHARSET) {
			switch (ascii) {
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
					ascii);
			}
			term->esc = 0;
		} else if (term->esc & ESC_TEST) {
			if (ascii == '8') {	/* DEC screen alignment test. */
				char E[UTF_SIZ] = "E";
				struct coord p;

				for (p.x = 0; p.x < term->size.x; ++p.x)
					for (p.y = 0; p.y < term->size.y; ++p.y)
						tsetchar(term, E, p);
			}
			term->esc = 0;
		} else {
			switch (ascii) {
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
				term->strescseq.type = ascii;
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
				xresettitle(xw);
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
					(unsigned char) ascii,
					isprint(ascii) ? ascii : '.');
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

	if (term->sel.bx != -1 &&
	    BETWEEN(term->c.pos.y, term->sel.by, term->sel.ey))
		term->sel.bx = -1;

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

static void techo(struct st_window *xw,
		  char *buf, int len)
{
	for (; len > 0; buf++, len--) {
		char c = *buf;

		if (c == '\033') {	/* escape */
			tputc(xw, "^", 1);
			tputc(xw, "[", 1);
		} else if (c < '\x20') {	/* control code */
			if (c != '\n' && c != '\r' && c != '\t') {
				c |= '\x40';
				tputc(xw, "^", 1);
			}
			tputc(xw, &c, 1);
		} else {
			break;
		}
	}
	if (len)
		tputc(xw, buf, len);
}

static char *kmap(struct st_term *term, KeySym k, unsigned state)
{
	unsigned mask;
	struct st_key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for (i = 0; i < ARRAY_SIZE(mappedkeys); i++) {
		if (mappedkeys[i] == k)
			break;
	}
	if (i == ARRAY_SIZE(mappedkeys)) {
		if ((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for (kp = key; kp < key + ARRAY_SIZE(key); kp++) {
		mask = kp->mask;

		if (kp->k != k)
			continue;

		if (!match(mask, state))
			continue;

		if (kp->appkey > 0) {
			if (!term->appkeypad)
				continue;
			if (term->numlock && kp->appkey == 2)
				continue;
		} else if (kp->appkey < 0 && term->appkeypad)
			continue;

		if ((kp->appcursor < 0 && term->appcursor) ||
		    (kp->appcursor > 0 && !term->appcursor))
			continue;

		if ((kp->crlf < 0 && term->crlf) ||
		    (kp->crlf > 0 && !term->crlf))
			continue;

		return kp->s;
	}

	return NULL;
}

static void kpress(struct st_window *xw, XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym;
	char xstr[31], buf[32], *customkey, *cp = buf;
	int len;
	Status status;
	struct st_shortcut *bp;

	if (xw->term.kbdlock)
		return;

	len = XmbLookupString(xw->xic, e, xstr, sizeof(xstr),
			      &ksym, &status);
	e->state &= ~Mod2Mask;
	/* 1. shortcuts */
	for (bp = shortcuts; bp < shortcuts + ARRAY_SIZE(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(xw, &(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if ((customkey = kmap(&xw->term, ksym, e->state))) {
		len = strlen(customkey);
		memcpy(buf, customkey, len);
		/* 2. hardcoded (overrides X lookup) */
	} else {
		if (len == 0)
			return;

		if (len == 1 && e->state & Mod1Mask)
			*cp++ = '\033';

		memcpy(cp, xstr, len);
		len = cp - buf + len;
	}

	ttywrite(&xw->term, buf, len);
	if (xw->term.echo)
		techo(xw, buf, len);
}

static void ttyread(struct st_window *xw)
{
	struct st_term *term = &xw->term;
	unsigned char *ptr;
	int ret;

	/* append read bytes to unprocessed bytes */
	if ((ret = read(term->cmdfd,
			term->cmdbuf + term->cmdbuflen,
			sizeof(term->cmdbuf) - term->cmdbuflen)) < 0)
		die("Couldn't read from shell: %s\n", SERRNO);

	if (iofd != -1 &&
	    xwrite(iofd, term->cmdbuf + term->cmdbuflen, ret) < 0) {
		fprintf(stderr, "Error writing in %s:%s\n",
			opt_io, strerror(errno));
		close(iofd);
		iofd = -1;
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

		tputc(xw, (char *) ptr, charsize);
		ptr += charsize;
		term->cmdbuflen -= charsize;
	}

	/* keep any uncomplete utf8 char for the next call */
	memmove(term->cmdbuf, ptr, term->cmdbuflen);
}

/* Mouse code */

static int x2col(struct st_window *xw, unsigned x)
{
	x -= borderpx;
	x /= xw->charsize.x;

	return min(x, xw->term.size.x - 1);
}

static int y2row(struct st_window *xw, unsigned y)
{
	y -= borderpx;
	y /= xw->charsize.y;

	return min(y, xw->term.size.y - 1);
}

static void getbuttoninfo(struct st_window *xw, XEvent *ev)
{
	int type;
	unsigned state = ev->xbutton.state & ~Button1Mask;
	struct st_selection *sel = &xw->term.sel;

	sel->alt = xw->term.altscreen;

	sel->ex = x2col(xw, ev->xbutton.x);
	sel->ey = y2row(xw, ev->xbutton.y);

	sel->b.x = sel->by < sel->ey ? sel->bx : sel->ex;
	sel->b.y = min(sel->by, sel->ey);
	sel->e.x = sel->by < sel->ey ? sel->ex : sel->bx;
	sel->e.y = max(sel->by, sel->ey);

	sel->type = SEL_REGULAR;
	for (type = 1; type < ARRAY_SIZE(selmasks); ++type) {
		if (match(selmasks[type], state)) {
			sel->type = type;
			break;
		}
	}
}

static void mousereport(struct st_window *xw, XEvent *ev)
{
	int button = ev->xbutton.button, state = ev->xbutton.state, len;
	char buf[40];
	struct coord pos = {
		x2col(xw, ev->xbutton.x),
		y2row(xw, ev->xbutton.y)
	};

	/* from urxvt */
	if (ev->xbutton.type == MotionNotify) {
		if (!xw->term.mousemotion ||
		    (pos.x == xw->mousepos.x &&
		     pos.y == xw->mousepos.y))
			return;

		button = xw->mousebutton + 32;
		xw->mousepos = pos;
	} else if (!xw->term.mousesgr &&
		   (ev->xbutton.type == ButtonRelease ||
		    button == AnyButton)) {
		button = 3;
	} else {
		button -= Button1;
		if (button >= 3)
			button += 64 - 3;
		if (ev->xbutton.type == ButtonPress) {
			xw->mousebutton = button;
			xw->mousepos = pos;
		}
	}

	button += (state & ShiftMask ? 4 : 0)
	    + (state & Mod4Mask ? 8 : 0)
	    + (state & ControlMask ? 16 : 0);

	len = 0;
	if (xw->term.mousesgr) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
			       button, pos.x + 1, pos.y + 1,
			       ev->xbutton.type ==
			       ButtonRelease ? 'm' : 'M');
	} else if (pos.x < 223 && pos.y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
			       32 + button, 32 + pos.x + 1, 32 + pos.y + 1);
	} else {
		return;
	}

	ttywrite(&xw->term, buf, len);
}

static void bpress(struct st_window *xw, XEvent *ev)
{
	struct st_term *term = &xw->term;
	struct st_selection *sel = &xw->term.sel;

	if (term->mousebtn || term->mousemotion) {
		mousereport(xw, ev);
	} else if (ev->xbutton.button == Button1) {
		if (sel->bx != -1) {
			sel->bx = -1;
			xw->term.dirty[sel->b.y] = 1;
			draw(xw);
		}
		sel->mode = 1;
		sel->type = SEL_REGULAR;
		sel->ex = sel->bx = x2col(xw, ev->xbutton.x);
		sel->ey = sel->by = y2row(xw, ev->xbutton.y);
	} else if (ev->xbutton.button == Button4) {
		ttywrite(term, "\031", 1);
	} else if (ev->xbutton.button == Button5) {
		ttywrite(term, "\005", 1);
	}
}

static void brelease(struct st_window *xw, XEvent *e)
{
	struct st_term *term = &xw->term;
	struct st_selection *sel = &term->sel;
	struct timeval now;

	if (term->mousebtn || term->mousemotion) {
		mousereport(xw, e);
		return;
	}

	if (e->xbutton.button == Button2) {
		selpaste(xw, NULL);
	} else if (e->xbutton.button == Button1) {
		sel->mode = 0;
		getbuttoninfo(xw, e);
		term->dirty[sel->ey] = 1;
		if (sel->bx == sel->ex && sel->by == sel->ey) {
			sel->bx = -1;
			gettimeofday(&now, NULL);

			if (TIMEDIFF(now, sel->tclick2) <=
			    tripleclicktimeout) {
				/* triple click on the line */
				sel->b.x = sel->bx = 0;
				sel->e.x = sel->ex = term->size.x;
				sel->b.y = sel->e.y = sel->ey;
				selcopy(xw);
			} else if (TIMEDIFF(now, sel->tclick1) <=
				   doubleclicktimeout) {
				/* double click to select word */
				sel->bx = sel->ex;
				while (sel->bx > 0 &&
				       term->line[sel->ey][sel->bx - 1].set &&
				       term->line[sel->ey][sel->bx - 1].c[0]
				       != ' ')
					sel->bx--;
				sel->b.x = sel->bx;
				while (sel->ex < term->size.x - 1 &&
				       term->line[sel->ey][sel->ex + 1].set &&
				       term->line[sel->ey][sel->ex + 1].c[0]
				       != ' ')
					sel->ex++;
				sel->e.x = sel->ex;
				sel->b.y = sel->e.y = sel->ey;
				selcopy(xw);
			}
		} else {
			selcopy(xw);
		}
	}

	memcpy(&sel->tclick2, &sel->tclick1, sizeof(struct timeval));
	gettimeofday(&sel->tclick1, NULL);
}

static void bmotion(struct st_window *xw, XEvent *e)
{
	struct st_term *term = &xw->term;
	int oldey, oldex, oldsby, oldsey;

	if (term->mousebtn || term->mousemotion) {
		mousereport(xw, e);
		return;
	}

	if (!term->sel.mode)
		return;

	oldey = term->sel.ey;
	oldex = term->sel.ex;
	oldsby = term->sel.b.y;
	oldsey = term->sel.e.y;
	getbuttoninfo(xw, e);

	if (oldey != term->sel.ey || oldex != term->sel.ex)
		tsetdirt(term,
			 min(term->sel.b.y, oldsby),
			 max(term->sel.e.y, oldsey));
}

/* Resizing code */

static void ttyresize(struct st_window *xw)
{
	struct winsize w;

	w.ws_row = xw->term.size.y;
	w.ws_col = xw->term.size.x;
	w.ws_xpixel = xw->ttysize.x;
	w.ws_ypixel = xw->ttysize.y;
	if (ioctl(xw->term.cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", SERRNO);
}

static int tresize(struct st_term *term, struct coord size)
{
	unsigned i, x;
	unsigned minrow = min(size.y, term->size.y);
	unsigned mincol = min(size.x, term->size.x);
	int slide = term->c.pos.y - size.y + 1;
	bool *bp;

	if (size.x < 1 || size.y < 1)
		return 0;

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
	term->dirty = xrealloc(term->dirty, size.y * sizeof(*term->dirty));
	term->tabs = xrealloc(term->tabs, size.x * sizeof(*term->tabs));

	/* resize each row to new width, zero-pad if needed */
	for (i = 0; i < minrow; i++) {
		term->dirty[i] = 1;
		term->line[i] = xrealloc(term->line[i], size.x * sizeof(struct st_glyph));
		term->alt[i] = xrealloc(term->alt[i], size.x * sizeof(struct st_glyph));
		for (x = mincol; x < size.x ; x++) {
			term->line[i][x].set = 0;
			term->alt[i][x].set = 0;
		}
	}

	/* allocate any new rows */
	for ( /* i == minrow */ ; i < size.y; i++) {
		term->dirty[i] = 1;
		term->line[i] = xcalloc(size.x, sizeof(struct st_glyph));
		term->alt[i] = xcalloc(size.x, sizeof(struct st_glyph));
	}
	if (size.x > term->size.x) {
		bp = term->tabs + term->size.x;

		memset(bp, 0, sizeof(*term->tabs) * (size.x - term->size.x));
		while (--bp > term->tabs && !*bp)
			/* nothing */ ;
		for (bp += tabspaces; bp < term->tabs + size.x;
		     bp += tabspaces)
			*bp = 1;
	}
	/* update terminal size */
	term->size = size;
	/* reset scrolling region */
	tsetscroll(term, 0, size.y - 1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(term, term->c.pos);

	return (slide > 0);
}

static void xresize(struct st_window *xw, int col, int row)
{
	xw->ttysize.x = max(1U, col * xw->charsize.x);
	xw->ttysize.y = max(1U, row * xw->charsize.y);

	XFreePixmap(xw->dpy, xw->buf);
	xw->buf = XCreatePixmap(xw->dpy, xw->win,
				xw->winsize.x, xw->winsize.y,
				DefaultDepth(xw->dpy, xw->scr));
	XSetForeground(xw->dpy, xw->gc,
		       xw->col[xw->term.reverse ? defaultfg : defaultbg].
		       pixel);
	XFillRectangle(xw->dpy, xw->buf, xw->gc, 0, 0,
		       xw->winsize.x, xw->winsize.y);

	XftDrawChange(xw->draw, xw->buf);
}

static void cresize(struct st_window *xw, unsigned width, unsigned height)
{
	struct coord size;

	if (width != 0)
		xw->winsize.x = width;
	if (height != 0)
		xw->winsize.y = height;

	size.x = (xw->winsize.x - 2 * borderpx) / xw->charsize.x;
	size.y = (xw->winsize.y - 2 * borderpx) / xw->charsize.y;

	tresize(&xw->term, size);
	xresize(xw, size.x, size.y);
	ttyresize(xw);
}

static void resize(struct st_window *xw, XEvent *ev)
{
	if (ev->xconfigure.width == xw->winsize.x &&
	    ev->xconfigure.height == xw->winsize.y)
		return;

	cresize(xw, ev->xconfigure.width, ev->xconfigure.height);
}

/* Start of st */

static void ttynew(struct st_window *xw)
{
	int m, s;
	struct winsize w = { xw->term.size.y, xw->term.size.x, 0, 0 };

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", SERRNO);

	switch (pid = fork()) {
	case -1:
		die("fork failed\n");
		break;
	case 0:
		setsid();	/* create a new process group */
		dup2(s, STDIN_FILENO);
		dup2(s, STDOUT_FILENO);
		dup2(s, STDERR_FILENO);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", SERRNO);
		close(s);
		close(m);
		execsh(xw->win);
		break;
	default:
		close(s);
		xw->term.cmdfd = m;
		signal(SIGCHLD, sigchld);
		if (opt_io) {
			iofd = (!strcmp(opt_io, "-")) ?
			    STDOUT_FILENO :
			    open(opt_io, O_WRONLY | O_CREAT, 0666);
			if (iofd < 0) {
				fprintf(stderr, "Error opening %s:%s\n",
					opt_io, strerror(errno));
			}
		}
	}
}

static void tnew(struct st_term *term, int col, int row)
{
	/* set screen size */
	term->size.y = row;
	term->size.x = col;
	term->line = xmalloc(term->size.y * sizeof(struct st_glyph *));
	term->alt = xmalloc(term->size.y * sizeof(struct st_glyph *));
	term->dirty = xmalloc(term->size.y * sizeof(*term->dirty));
	term->tabs = xmalloc(term->size.x * sizeof(*term->tabs));

	for (row = 0; row < term->size.y; row++) {
		term->line[row] = xmalloc(term->size.x * sizeof(struct st_glyph));
		term->alt[row] = xmalloc(term->size.x * sizeof(struct st_glyph));
		term->dirty[row] = 0;
	}

	term->numlock = 1;
	memset(term->tabs, 0, term->size.x * sizeof(*term->tabs));
	/* setup screen */
	treset(term);
}

static void selinit(struct st_window *xw)
{
	xw->term.sel.bx = -1;
	xw->term.sel.xtarget = XInternAtom(xw->dpy, "UTF8_STRING", 0);
	if (xw->term.sel.xtarget == None)
		xw->term.sel.xtarget = XA_STRING;
}

static void xloadcolors(struct st_window *xw)
{
	int i, r, g, b;
	XRenderColor color = {.alpha = 0xffff };

	/* load colors [0-15] colors and [256-ARRAY_SIZE(colorname)[ (config.h) */
	for (i = 0; i < ARRAY_SIZE(colorname); i++) {
		if (!colorname[i])
			continue;
		if (!XftColorAllocName(xw->dpy, xw->vis, xw->cmap,
				       colorname[i], &xw->col[i]))
			die("Could not allocate color '%s'\n", colorname[i]);
	}

	/* load colors [16-255] ; same colors as xterm */
	for (i = 16, r = 0; r < 6; r++) {
		for (g = 0; g < 6; g++) {
			for (b = 0; b < 6; b++) {
				color.red = sixd_to_16bit(r);
				color.green = sixd_to_16bit(g);
				color.blue = sixd_to_16bit(b);
				if (!XftColorAllocValue(xw->dpy, xw->vis,
							xw->cmap, &color,
							&xw->col[i]))
					die("Could not allocate color %d\n", i);
				i++;
			}
		}
	}

	for (r = 0; r < 24; r++, i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
		if (!XftColorAllocValue(xw->dpy, xw->vis, xw->cmap,
					&color, &xw->col[i]))
			die("Could not allocate color %d\n", i);
	}
}

static void xhints(struct st_window *xw)
{
	XClassHint class = { xw->class, termname };
	XWMHints wm = {.flags = InputHint,.input = 1 };
	XSizeHints *sizeh = NULL;

	sizeh = XAllocSizeHints();
	if (xw->isfixed == False) {
		sizeh->flags = PSize | PResizeInc | PBaseSize;
		sizeh->width = xw->winsize.x;
		sizeh->height = xw->winsize.y;
		sizeh->width_inc = xw->charsize.x;
		sizeh->height_inc = xw->charsize.y;
		sizeh->base_height = 2 * borderpx;
		sizeh->base_width = 2 * borderpx;
	} else {
		sizeh->flags = PMaxSize | PMinSize;
		sizeh->min_width = sizeh->max_width = xw->fixedsize.x;
		sizeh->min_height = sizeh->max_height = xw->fixedsize.y;
	}

	XSetWMProperties(xw->dpy, xw->win, NULL, NULL, NULL,
			 0, sizeh, &wm, &class);
	XFree(sizeh);
}

static int xloadfont(struct st_window *xw, struct st_font *f,
		     FcPattern *pattern)
{
	FcPattern *match;
	FcResult result;

	match = FcFontMatch(NULL, pattern, &result);
	if (!match)
		return 1;

	if (!(f->set = FcFontSort(0, match, FcTrue, 0, &result))) {
		FcPatternDestroy(match);
		return 1;
	}

	if (!(f->match = XftFontOpenPattern(xw->dpy, match))) {
		FcPatternDestroy(match);
		return 1;
	}

	f->pattern = FcPatternDuplicate(pattern);

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->match->height;
	f->width = f->lbearing + f->rbearing;

	return 0;
}

static void xloadfonts(struct st_window *xw,
		       char *fontstr, int fontsize)
{
	FcPattern *pattern;
	FcResult result;
	double fontval;

	if (fontstr[0] == '-') {
		pattern = XftXlfdParse(fontstr, False, False);
	} else {
		pattern = FcNameParse((FcChar8 *) fontstr);
	}

	if (!pattern)
		die("st: can't open font %s\n", fontstr);

	if (fontsize > 0) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE,
				   (double) fontsize);
		usedfontsize = fontsize;
	} else {
		result =
		    FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0,
				       &fontval);
		if (result == FcResultMatch) {
			usedfontsize = (int) fontval;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
	}

	FcConfigSubstitute(0, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	if (xloadfont(xw, &xw->font, pattern))
		die("st: can't open font %s\n", fontstr);

	/* Setting character width and height. */
	xw->charsize.x = xw->font.width;
	xw->charsize.y = xw->font.height;

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (xloadfont(xw, &xw->ifont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (xloadfont(xw, &xw->ibfont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (xloadfont(xw, &xw->bfont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

static void xunloadfonts(struct st_window *xw)
{
	int i, ip;

	/*
	 * Free the loaded fonts in the font cache. This is done backwards
	 * from the frccur.
	 */
	for (i = 0, ip = frccur; i < frclen; i++, ip--) {
		if (ip < 0)
			ip = ARRAY_SIZE(frc) - 1;
		XftFontClose(xw->dpy, frc[ip].font);
	}
	frccur = -1;
	frclen = 0;

	XftFontClose(xw->dpy, xw->font.match);
	FcPatternDestroy(xw->font.pattern);
	FcFontSetDestroy(xw->font.set);
	XftFontClose(xw->dpy, xw->bfont.match);
	FcPatternDestroy(xw->bfont.pattern);
	FcFontSetDestroy(xw->bfont.set);
	XftFontClose(xw->dpy, xw->ifont.match);
	FcPatternDestroy(xw->ifont.pattern);
	FcFontSetDestroy(xw->ifont.set);
	XftFontClose(xw->dpy, xw->ibfont.match);
	FcPatternDestroy(xw->ibfont.pattern);
	FcFontSetDestroy(xw->ibfont.set);
}

static void xzoom(struct st_window *xw, const union st_arg *arg)
{
	xunloadfonts(xw);
	xloadfonts(xw, usedfont, usedfontsize + arg->i);
	cresize(xw, 0, 0);
	redraw(xw, 0);
}

static void xinit(struct st_window *xw)
{
	XSetWindowAttributes attrs;
	XGCValues gcvalues;
	Cursor cursor;
	Window parent;
	int sw, sh;

	if (!(xw->dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw->scr = XDefaultScreen(xw->dpy);
	xw->vis = XDefaultVisual(xw->dpy, xw->scr);

	/* font */
	if (!FcInit())
		die("Could not init fontconfig.\n");

	usedfont = (opt_font == NULL) ? font : opt_font;
	xloadfonts(xw, usedfont, 0);

	/* colors */
	xw->cmap = XDefaultColormap(xw->dpy, xw->scr);
	xloadcolors(xw);

	/* adjust fixed window geometry */
	if (xw->isfixed) {
		sw = DisplayWidth(xw->dpy, xw->scr);
		sh = DisplayHeight(xw->dpy, xw->scr);
		if (xw->fx < 0)
			xw->fx = sw + xw->fx - xw->fixedsize.x - 1;
		if (xw->fy < 0)
			xw->fy = sh + xw->fy - xw->fixedsize.y - 1;

		xw->winsize = xw->fixedsize;
	} else {
		/* window - default size */
		xw->winsize.x = 2 * borderpx + xw->term.size.x * xw->charsize.x;
		xw->winsize.y = 2 * borderpx + xw->term.size.y * xw->charsize.y;
		xw->fixedsize.x = 0;
		xw->fixedsize.y = 0;
	}

	/* Events */
	attrs.background_pixel = xw->col[defaultbg].pixel;
	attrs.border_pixel = xw->col[defaultbg].pixel;
	attrs.bit_gravity = NorthWestGravity;
	attrs.event_mask = FocusChangeMask | KeyPressMask
	    | ExposureMask | VisibilityChangeMask | StructureNotifyMask
	    | ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	attrs.colormap = xw->cmap;

	parent = xw->embed ? strtol(xw->embed, NULL, 0) :
	    XRootWindow(xw->dpy, xw->scr);
	xw->win = XCreateWindow(xw->dpy, parent, xw->fx, xw->fy,
				xw->winsize.x, xw->winsize.y, 0,
				XDefaultDepth(xw->dpy, xw->scr),
				InputOutput, xw->vis,
				CWBackPixel | CWBorderPixel | CWBitGravity |
				CWEventMask | CWColormap, &attrs);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	xw->gc = XCreateGC(xw->dpy, parent, GCGraphicsExposures, &gcvalues);
	xw->buf = XCreatePixmap(xw->dpy, xw->win, xw->winsize.x, xw->winsize.y,
				DefaultDepth(xw->dpy, xw->scr));
	XSetForeground(xw->dpy, xw->gc, xw->col[defaultbg].pixel);
	XFillRectangle(xw->dpy, xw->buf, xw->gc, 0, 0,
		       xw->winsize.x, xw->winsize.y);

	/* Xft rendering context */
	xw->draw = XftDrawCreate(xw->dpy, xw->buf,
				    xw->vis, xw->cmap);

	/* input methods */
	if ((xw->xim = XOpenIM(xw->dpy, NULL, NULL, NULL)) == NULL) {
		XSetLocaleModifiers("@im=local");
		if ((xw->xim = XOpenIM(xw->dpy, NULL, NULL, NULL)) == NULL) {
			XSetLocaleModifiers("@im=");
			if ((xw->xim = XOpenIM(xw->dpy,
					      NULL, NULL, NULL)) == NULL) {
				die("XOpenIM failed. Could not open input"
				    " device.\n");
			}
		}
	}
	xw->xic = XCreateIC(xw->xim, XNInputStyle, XIMPreeditNothing
			   | XIMStatusNothing, XNClientWindow, xw->win,
			   XNFocusWindow, xw->win, NULL);
	if (xw->xic == NULL)
		die("XCreateIC failed. Could not obtain input method.\n");

	/* white cursor, black outline */
	cursor = XCreateFontCursor(xw->dpy, XC_xterm);
	XDefineCursor(xw->dpy, xw->win, cursor);
	XRecolorCursor(xw->dpy, cursor, &(XColor) {
		       .red = 0xffff,.green = 0xffff,.blue = 0xffff}
		       , &(XColor) {
		       .red = 0x0000,.green = 0x0000,.blue = 0x0000}
	);

	xw->xembed = XInternAtom(xw->dpy, "_XEMBED", False);
	xw->wmdeletewin = XInternAtom(xw->dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(xw->dpy, xw->win, &xw->wmdeletewin, 1);

	xresettitle(xw);
	XMapWindow(xw->dpy, xw->win);
	xhints(xw);
	XSync(xw->dpy, 0);
}

static void expose(struct st_window *xw, XEvent *ev)
{
	XExposeEvent *e = &ev->xexpose;

	if (xw->redraw && !e->count)
		xw->redraw = 0;
	redraw(xw, 0);
}

static void visibility(struct st_window *xw, XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;

	if (e->state == VisibilityFullyObscured) {
		xw->visible = 0;
	} else if (!xw->visible) {
		/* need a full redraw for next Expose, not just a buf copy */
		xw->visible = 1;
		xw->redraw = 1;
	}
}

static void unmap(struct st_window *xw, XEvent *ev)
{
	xw->visible = 0;
}

static void focus(struct st_window *xw, XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab)
		return;

	if (ev->type == FocusIn) {
		XSetICFocus(xw->xic);
		xw->focused = 1;
		xseturgency(xw, 0);
	} else {
		XUnsetICFocus(xw->xic);
		xw->focused = 0;
	}
}

static void numlock(struct st_window *xw, const union st_arg *dummy)
{
	xw->term.numlock ^= 1;
}

static void cmessage(struct st_window *xw, XEvent *ev)
{
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if (ev->xclient.message_type == xw->xembed
	    && ev->xclient.format == 32) {
		if (ev->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			xw->focused = 1;
			xseturgency(xw, 0);
		} else if (ev->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			xw->focused = 0;
		}
	} else if (ev->xclient.data.l[0] == xw->wmdeletewin) {
		/* Send SIGHUP to shell */
		kill(pid, SIGHUP);
		exit(EXIT_SUCCESS);
	}
}

static void run(struct st_window *xw)
{
	XEvent ev;
	fd_set rfd;
	int xfd = XConnectionNumber(xw->dpy), xev = actionfps;
	struct timeval drawtimeout, *tv = NULL, now, last;

	void (*handler[LASTEvent]) (struct st_window *, XEvent *) = {
		[KeyPress] = kpress,
		[ClientMessage] = cmessage,
		[ConfigureNotify] = resize,
		[VisibilityNotify] = visibility,
		[UnmapNotify] = unmap,
		[Expose] = expose,
		[FocusIn] = focus,
		[FocusOut] = focus,
		[MotionNotify] = bmotion,
		[ButtonPress] = bpress,
		[ButtonRelease] = brelease,
		[SelectionClear] = selclear,
		[SelectionNotify] = selnotify,
		[SelectionRequest] = selrequest,};

	gettimeofday(&last, NULL);

	while (1) {
		FD_ZERO(&rfd);
		FD_SET(xw->term.cmdfd, &rfd);
		FD_SET(xfd, &rfd);
		if (select(max(xfd, xw->term.cmdfd) + 1, &rfd, NULL, NULL, tv) < 0) {
			if (errno == EINTR)
				continue;
			die("select failed: %s\n", SERRNO);
		}

		gettimeofday(&now, NULL);
		drawtimeout.tv_sec = 0;
		drawtimeout.tv_usec = (1000 / xfps) * 1000;
		tv = &drawtimeout;

		if (FD_ISSET(xw->term.cmdfd, &rfd))
			ttyread(xw);

		if (FD_ISSET(xfd, &rfd))
			xev = actionfps;

		if (TIMEDIFF(now, last) >
		    (xev ? (1000 / xfps) : (1000 / actionfps))) {
			while (XPending(xw->dpy)) {
				XNextEvent(xw->dpy, &ev);
				if (XFilterEvent(&ev, None))
					continue;
				if (handler[ev.type])
					(handler[ev.type])(xw, &ev);
			}

			draw(xw);
			XFlush(xw->dpy);
			last = now;

			if (xev && !FD_ISSET(xfd, &rfd))
				xev--;
			if (!FD_ISSET(xw->term.cmdfd, &rfd) && !FD_ISSET(xfd, &rfd))
				tv = NULL;
		}
	}
}

int main(int argc, char *argv[])
{
	int i, bitm, xr, yr;
	unsigned wr, hr;
	struct st_window xw;

	memset(&xw, 0, sizeof(xw));

	xw.default_title	= "st";
	xw.class		= termname;

	for (i = 1; i < argc; i++) {
		switch (argv[i][0] != '-' || argv[i][2] ? -1 : argv[i][1]) {
		case 'c':
			if (++i < argc)
				xw.class = argv[i];
			break;
		case 'e':
			/* eat all remaining arguments */
			if (++i < argc)
				opt_cmd = &argv[i];
			goto run;
		case 'f':
			if (++i < argc)
				opt_font = argv[i];
			break;
		case 'g':
			if (++i >= argc)
				break;

			bitm = XParseGeometry(argv[i], &xr, &yr, &wr, &hr);
			if (bitm & XValue)
				xw.fx = xr;
			if (bitm & YValue)
				xw.fy = yr;
			if (bitm & WidthValue)
				xw.fixedsize.x = (int) wr;
			if (bitm & HeightValue)
				xw.fixedsize.y = (int) hr;
			if (bitm & XNegative && xw.fx == 0)
				xw.fx = -1;
			if (bitm & XNegative && xw.fy == 0)
				xw.fy = -1;

			if (xw.fixedsize.x != 0 && xw.fixedsize.y != 0)
				xw.isfixed = True;
			break;
		case 'o':
			if (++i < argc)
				opt_io = argv[i];
			break;
		case 't':
			if (++i < argc)
				xw.default_title = argv[i];
			break;
		case 'v':
		default:
			die(USAGE);
		case 'w':
			if (++i < argc)
				xw.embed = argv[i];
			break;
		}
	}

      run:
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	tnew(&xw.term, 80, 24);
	xinit(&xw);
	ttynew(&xw);
	selinit(&xw);
	run(&xw);

	return 0;
}
