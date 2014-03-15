/* See LICENSE for licence details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>

#include "term.h"

#define USAGE \
	"st " VERSION " (c) 2010-2013 st engineers\n" \
	"usage: st [-v] [-c class] [-f font] [-g geometry] [-o file]" \
	" [-t title] [-w windowid] [-e command ...]\n"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* Arbitrary sizes */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

/* macros */
#define TIMEDIFF(t1, t2) ((t1.tv_sec-t2.tv_sec)*1000 + (t1.tv_usec-t2.tv_usec)/1000)

struct st_font {
	XftFont		*match;
	FcFontSet	*set;
	FcPattern	*pattern;
};

struct st_fontcache {
	XftFont		*font;
	unsigned	c;
	enum {
		FRC_NORMAL,
		FRC_ITALIC,
		FRC_BOLD,
		FRC_ITALICBOLD
	} flags;
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
	Atom		selection;
	char		*default_title;
	char		*class;
	char		*embed;

	struct st_font	font, bfont, ifont, ibfont;
	char		*fontname;
	int		fontzoom;
	struct st_fontcache fontcache[32];

	int		scr;
	bool		isfixed;	/* is fixed geometry? */
	int		fx, fy;		/* fixed geometry */
	struct coord	winsize;
	struct coord	fixedsize;	/* kill? */
	struct coord	charsize;

	struct coord	mousepos;
	unsigned	mousebutton;
	struct timeval	mousedown;
	struct timeval	mouseup[3];

	unsigned	mousemotion:1;
	unsigned	visible:1;
	unsigned	focused:1;
};

/* X utility code */

static unsigned short sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

static bool match(unsigned mask, unsigned state)
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

static int xsetcolorname(struct st_term *term,
			 int x, const char *name)
{
	struct st_window *xw = container_of(term, struct st_window, term);

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

static void xsettitle(struct st_term *term, char *title)
{
	struct st_window *xw = container_of(term, struct st_window, term);

	if (!title)
		title = xw->default_title;

	XTextProperty prop;

	Xutf8TextListToTextProperty(xw->dpy, &title, 1, XUTF8StringStyle,
				    &prop);
	XSetWMName(xw->dpy, xw->win, &prop);
}

static void xseturgency(struct st_term *term, int add)
{
	struct st_window *xw = container_of(term, struct st_window, term);

	if (xw->focused)
		return;

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

/* Selection code */

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
	XConvertSelection(xw->dpy, XA_PRIMARY, xw->selection,
			  XA_PRIMARY, xw->win, CurrentTime);
}

static void clippaste(struct st_window *xw, const union st_arg *dummy)
{
	Atom clipboard;

	clipboard = XInternAtom(xw->dpy, "CLIPBOARD", 0);
	XConvertSelection(xw->dpy, clipboard, xw->selection,
			  XA_PRIMARY, xw->win, CurrentTime);
}

static void selclear(struct st_window *xw, XEvent *e)
{
	term_sel_start(&xw->term, SEL_NONE, ORIGIN);
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
		string = xw->selection;
		XChangeProperty(xsre->display, xsre->requestor,
				xsre->property, XA_ATOM, 32,
				PropModeReplace, (unsigned char *) & string, 1);
		xev.property = xsre->property;
	} else if (xsre->target == xw->selection && sel->clip != NULL) {
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

/* Screen drawing code */

static void xclear(struct st_window *xw, XftColor *color,
		   struct coord pos, unsigned charlen,
		   bool clear_border)
{
	unsigned x1 = xw->charsize.x * pos.x + borderpx;
	unsigned x2 = xw->charsize.x * charlen;
	unsigned y1 = xw->charsize.y * pos.y + borderpx;
	unsigned y2 = xw->charsize.y;

	if (clear_border) {
		if (!pos.x) {
			x2 += x1;
			x1 = 0;
		}

		if (pos.x + charlen == xw->term.size.x)
			x2 = xw->winsize.x - x1;

		if (!pos.y) {
			y2 += y1;
			y1 = 0;
		}

		if (pos.y + 1 == xw->term.size.y)
			y2 = xw->winsize.y - y1;
	}

	/* Clean up the region we want to draw to. */
	XftDrawRect(xw->draw, color, x1, y1, x2, y2);
}

static XftColor *reverse_color(struct st_window *xw, XftColor *color,
			       XftColor *def, XftColor *defreverse,
			       XftColor *reverse)
{
	if (color == def) {
		return defreverse;
	} else {
		XRenderColor t;

		t.red = ~color->color.red;
		t.green = ~color->color.green;
		t.blue = ~color->color.blue;
		t.alpha = color->color.alpha;
		XftColorAllocValue(xw->dpy, xw->vis, xw->cmap, &t, reverse);

		return reverse;
	}
}

static XftFont *find_font(struct st_window *xw, struct st_font *font,
			  unsigned flags, long u8char)
{
	FcFontSet *fcsets[] = { font->set };
	FcPattern *fcpattern, *fontpattern;
	FcCharSet *fccharset;
	FcResult fcres;
	XftFont *xfont;
	struct st_fontcache *fc;

	/* Search the font cache. */
	for (fc = xw->fontcache;
	     fc < xw->fontcache + ARRAY_SIZE(xw->fontcache) && fc->font;
	     fc++)
		if (fc->flags == flags && fc->c == u8char)
			return fc->font;

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

	fontpattern = FcFontSetMatch(NULL, fcsets, 1, fcpattern, &fcres);

	xfont = XftFontOpenPattern(xw->dpy, fontpattern);

	fc = &xw->fontcache[ARRAY_SIZE(xw->fontcache) - 1];
	if (fc->font)
		XftFontClose(xw->dpy, fc->font);

	fc = xw->fontcache;
	memmove(fc + 1, fc,
		(ARRAY_SIZE(xw->fontcache) - 1) * sizeof(*fc));

	fc->font = xfont;
	fc->c = u8char;
	fc->flags = flags;

	FcCharSetDestroy(fccharset);
	FcPatternDestroy(fcpattern);

	return xfont;
}

static void xdraw_glyphs(struct st_window *xw, struct coord pos,
			 struct st_glyph base, struct st_glyph *glyphs,
			 unsigned charlen, bool clear_border)
{
	unsigned winx = borderpx + pos.x * xw->charsize.x, xp = winx;
	unsigned winy = borderpx + pos.y * xw->charsize.y;
	unsigned frcflags = FRC_NORMAL;
	struct st_font *font = &xw->font;
	unsigned xglyphs[1024], nxglyphs = 0;
	XftColor *fg = &xw->col[base.fg];
	XftColor *bg = &xw->col[base.bg];
	XftColor revfg, revbg;

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
		fg = reverse_color(xw, fg, &xw->col[defaultfg],
				   &xw->col[defaultbg], &revfg);

		bg = reverse_color(xw, bg, &xw->col[defaultbg],
				   &xw->col[defaultfg], &revbg);
	}

	if (base.reverse)
		swap(bg, fg);

	xclear(xw, bg, pos, charlen, clear_border);

	for (unsigned i = 0; i < charlen; i++) {
		/*
		 * Search for the range in the to be printed string of glyphs
		 * that are in the main font. Then print that range. If
		 * some glyph is found that is not in the font, do the
		 * fallback dance.
		 */
		XftFont *xfont = font->match;
		bool found;
		unsigned ucs = glyphs[i].c;

		if (!ucs)
			ucs = ' ';
retry:
		xglyphs[nxglyphs] = XftCharIndex(xw->dpy, xfont, ucs);
		found = xglyphs[nxglyphs];

		if (found)
			nxglyphs++;

		if ((!found && nxglyphs) || nxglyphs == ARRAY_SIZE(xglyphs)) {
			XftDrawGlyphs(xw->draw, fg, xfont, xp,
				      winy + xfont->ascent, xglyphs, nxglyphs);
			xp += xw->charsize.x * nxglyphs;
			nxglyphs = 0;
		}

		if (!found) {
			xfont = find_font(xw, font, ucs, frcflags);
			if (!xfont) {
				if (ucs != 0xFFFD)
					ucs = 0xFFFD;
				else
					ucs = ' ';
				goto retry;
			}

			xglyphs[nxglyphs] = XftCharIndex(xw->dpy, xfont, ucs);

			XftDrawGlyphs(xw->draw, fg, xfont, xp,
				      winy + xfont->ascent, xglyphs, 1);

			xp += xw->charsize.x;
		}
	}

	if (nxglyphs)
		XftDrawGlyphs(xw->draw, fg, font->match, xp,
			      winy + font->match->ascent, xglyphs, nxglyphs);

	if (base.underline)
		XftDrawRect(xw->draw, fg, winx, winy + font->match->ascent + 1,
			    charlen * xw->charsize.x, 1);
}

static void xdrawcursor(struct st_window *xw)
{
	struct st_glyph g;

	if (xw->term.hide)
		return;

	g.c = term_pos(&xw->term, xw->term.c.pos)->c;
	g.cmp = 0;
	g.fg = defaultbg;
	g.bg = defaultcs;

	g.reverse = xw->term.reverse;
	if (g.reverse) {
		unsigned t = g.fg;
		g.fg = g.bg;
		g.bg = t;
	}

	if (xw->focused) {
		xdraw_glyphs(xw, xw->term.c.pos, g, &g, 1, false);
	} else {
		XSetForeground(xw->dpy, xw->gc, xw->col[defaultcs].pixel);
		XDrawRectangle(xw->dpy, xw->buf, xw->gc,
			       borderpx + xw->term.c.pos.x * xw->charsize.x,
			       borderpx + xw->term.c.pos.y * xw->charsize.y,
			       xw->charsize.x, xw->charsize.y);
	}
}

static struct st_glyph sel_glyph(struct st_window *xw, unsigned x, unsigned y)
{
	struct st_glyph ret = xw->term.line[y][x];

	if (term_selected(&xw->term.sel, x, y))
		ret.reverse ^= 1;

	return ret;
}

static void draw(struct st_window *xw)
{
	struct coord pos;

	if (!xw->visible)
		return;

	if (!xw->term.dirty)
		return;

	xw->term.dirty = false;

	for (pos.y = 0; pos.y < xw->term.size.y; pos.y++) {
		pos.x = 0;

		while (pos.x < xw->term.size.x) {
			unsigned x2 = pos.x + 1;
			struct st_glyph base = sel_glyph(xw, pos.x, pos.y);

			while (x2 < xw->term.size.x &&
			       base.cmp == sel_glyph(xw, x2, pos.y).cmp)
				x2++;

			xdraw_glyphs(xw, pos, base,
				     term_pos(&xw->term, pos), x2 - pos.x,
				     true);
			pos.x = x2;
		}
	}

	xdrawcursor(xw);

	XCopyArea(xw->dpy, xw->buf, xw->win, xw->gc,
		  0, 0, xw->winsize.x, xw->winsize.y, 0, 0);
	XSetForeground(xw->dpy, xw->gc,
		       xw->col[xw->term.reverse ? defaultfg : defaultbg].pixel);
	XFlush(xw->dpy);
}

/* Keyboard input */

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
	const struct st_shortcut *bp;

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
		term_echo(&xw->term, buf, len);
}

/* Mouse code */

static struct coord mouse_pos(struct st_window *xw, XEvent *ev)
{
	return (struct coord) {
		.x = min((ev->xbutton.x - borderpx) / xw->charsize.x,
			 xw->term.size.x - 1),
		.y = min((ev->xbutton.y - borderpx) / xw->charsize.y,
			 xw->term.size.y - 1),
	};
}

static void mousereport(struct st_window *xw, XEvent *ev)
{
	char buf[40];
	int button = ev->xbutton.button, state = ev->xbutton.state, len;
	struct coord pos = mouse_pos(xw, ev);

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

	button += (state & ShiftMask ? 4 : 0) +
		(state & Mod4Mask ? 8 : 0) +
		(state & ControlMask ? 16 : 0);

	if (xw->term.mousesgr)
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
			       button, pos.x + 1, pos.y + 1,
			       ev->xbutton.type ==
			       ButtonRelease ? 'm' : 'M');
	else if (pos.x < 223 && pos.y < 223)
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
			       32 + button, 32 + pos.x + 1, 32 + pos.y + 1);
	else
		return;

	ttywrite(&xw->term, buf, len);
}

static void bpress(struct st_window *xw, XEvent *ev)
{
	struct st_term *term = &xw->term;
	unsigned type, state;

	if (term->mousebtn || term->mousemotion) {
		mousereport(xw, ev);
		return;
	}

	switch (ev->xbutton.button) {
	case Button1:
		type = SEL_REGULAR;
		state = ev->xbutton.state & ~Button1Mask;

		for (unsigned i = 1; i < ARRAY_SIZE(selmasks); i++)
			if (match(selmasks[i], state)) {
				type = i;
				break;
			}

		xw->mousemotion = 1;
		term_sel_start(term, type, mouse_pos(xw, ev));
		gettimeofday(&xw->mousedown, NULL);
		break;
	case Button4:
		ttywrite(term, "\031", 1);
		break;
	case Button5:
		ttywrite(term, "\005", 1);
		break;
	}
}

static void brelease(struct st_window *xw, XEvent *ev)
{
	struct st_term *term = &xw->term;
	struct st_selection *sel = &term->sel;

	if (term->mousebtn || term->mousemotion) {
		mousereport(xw, ev);
		return;
	}

	switch (ev->xbutton.button) {
	case Button1:
		xw->mousemotion = 0;

		memmove(xw->mouseup + 1,
			xw->mouseup, sizeof(struct timeval) * 2);
		gettimeofday(xw->mouseup, NULL);

		struct coord end = mouse_pos(xw, ev);

		if (!(sel->start.x == end.x &&
		      sel->start.y == end.y))
			term_sel_update(term, end);
		else if (TIMEDIFF(xw->mouseup[0], xw->mouseup[2]) <
			   tripleclicktimeout)
			term_sel_line(term, end);
		else if (TIMEDIFF(xw->mouseup[0], xw->mouseup[1]) <
			 doubleclicktimeout)
			term_sel_word(term, end);
		else if (TIMEDIFF(xw->mouseup[0], xw->mousedown) <
			 doubleclicktimeout)
			term_sel_stop(term);

		if (sel->clip)
			xsetsel(xw);
		break;
	case Button2:
		selpaste(xw, NULL);
		break;
	}
}

static void bmotion(struct st_window *xw, XEvent *ev)
{
	struct st_term *term = &xw->term;

	if (term->mousebtn || term->mousemotion) {
		mousereport(xw, ev);
		return;
	}

	if (xw->mousemotion)
		term_sel_update(term, mouse_pos(xw, ev));
}

/* Resizing code */

static void xresize(struct st_window *xw, int col, int row)
{
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

	/* XXX: should probably be elsewhere */
	xw->term.ttysize.x = max(1U, size.x * xw->charsize.x);
	xw->term.ttysize.y = max(1U, size.y * xw->charsize.y);

	term_resize(&xw->term, size);
	xresize(xw, size.x, size.y);
}

static void resize(struct st_window *xw, XEvent *ev)
{
	if (ev->xconfigure.width == xw->winsize.x &&
	    ev->xconfigure.height == xw->winsize.y)
		return;

	cresize(xw, ev->xconfigure.width, ev->xconfigure.height);
}

/* Start of st */

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
	XClassHint class = { xw->class, TERMNAME };
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

	return 0;
}

static void xloadfonts(struct st_window *xw, const char *fontstr, int zoom)
{
	FcPattern *pattern;
	double pixelsize;

	if (fontstr[0] == '-')
		pattern = XftXlfdParse(fontstr, False, False);
	else
		pattern = FcNameParse((FcChar8 *) fontstr);

	if (!pattern)
		die("st: can't open font %s\n", fontstr);

	FcConfigSubstitute(0, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE,
			       0, &pixelsize) == FcResultMatch)
		FcPatternDel(pattern, FC_PIXEL_SIZE);
	else
		/*
		 * Default font size is 12, if none given. This is to
		 * have a known usedfontsize value.
		 */
		pixelsize = 12;

	pixelsize *= exp((double) zoom / 8);
	FcPatternAddDouble(pattern, FC_PIXEL_SIZE, pixelsize);

	if (xloadfont(xw, &xw->font, pattern))
		die("st: can't open font %s\n", fontstr);

	/* Setting character width and height. */
	xw->charsize.x = xw->font.match->max_advance_width;
	xw->charsize.y = xw->font.match->height;

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
	/*
	 * Free the loaded fonts in the font cache. This is done backwards
	 * from the frccur.
	 */
	for (unsigned i = 0; i < ARRAY_SIZE(xw->fontcache); i++)
		if (xw->fontcache[i].font)
			XftFontClose(xw->dpy, xw->fontcache[i].font);

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

__attribute((unused))
static void xzoom(struct st_window *xw, const union st_arg *arg)
{
	xw->fontzoom = clamp(xw->fontzoom + arg->i, -8, 8);

	xunloadfonts(xw);
	xloadfonts(xw, xw->fontname, xw->fontzoom);
	cresize(xw, 0, 0);
	xw->term.dirty = true;
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

	xloadfonts(xw, xw->fontname, 0);

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

	xw->selection = XInternAtom(xw->dpy, "UTF8_STRING", 0);
	if (xw->selection == None)
		xw->selection = XA_STRING;

	xsettitle(&xw->term, NULL);
	XMapWindow(xw->dpy, xw->win);
	xhints(xw);
	XSync(xw->dpy, 0);
}

static void expose(struct st_window *xw, XEvent *ev)
{
	xw->term.dirty = true;
}

static void visibility(struct st_window *xw, XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;

	if (e->state == VisibilityFullyObscured) {
		xw->visible = 0;
	} else if (!xw->visible) {
		/* need a full redraw for next Expose, not just a buf copy */
		xw->visible = 1;
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
		xw->term.dirty = true;
		xseturgency(&xw->term, 0);
	} else {
		XUnsetICFocus(xw->xic);
		xw->term.dirty = true;
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
			xseturgency(&xw->term, 0);
		} else if (ev->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			xw->focused = 0;
		}
	} else if (ev->xclient.data.l[0] == xw->wmdeletewin) {
		/* Send SIGHUP to shell */
		term_shutdown(&xw->term);
		exit(EXIT_SUCCESS);
	}
}

static void run(struct st_window *xw)
{
	XEvent ev;
	fd_set rfd;
	int xfd = XConnectionNumber(xw->dpy);
	struct timeval now, next_redraw, timeout, *tv = NULL;

	void (*handler[]) (struct st_window *, XEvent *) = {
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

	gettimeofday(&next_redraw, NULL);

	while (1) {
		FD_ZERO(&rfd);
		FD_SET(xw->term.cmdfd, &rfd);
		FD_SET(xfd, &rfd);

		if (select(max(xfd, xw->term.cmdfd) + 1,
			   &rfd, NULL, NULL, tv) < 0) {
			if (errno == EINTR)
				continue;
			edie("select failed");
		}

		if (FD_ISSET(xw->term.cmdfd, &rfd))
			term_read(&xw->term);

		while (XPending(xw->dpy)) {
			XNextEvent(xw->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;

			if (ev.type < ARRAY_SIZE(handler) &&
			    handler[ev.type])
				(handler[ev.type])(xw, &ev);
		}

		gettimeofday(&now, NULL);

		timeout.tv_sec = next_redraw.tv_sec - now.tv_sec;
		timeout.tv_usec = next_redraw.tv_usec - now.tv_usec;

		while (timeout.tv_usec < 0) {
			timeout.tv_usec += 1000000;
			timeout.tv_sec--;
		}

		if (timeout.tv_sec < 0) {
			draw(xw);
			next_redraw = now;
			next_redraw.tv_usec += 1000 * 1000 / xfps;
			tv = NULL;
		} else {
			tv = &timeout;
		}
	}
}

int main(int argc, char *argv[])
{
	int i, bitm, xr, yr;
	unsigned wr, hr;
	struct st_window xw;
	char **opt_cmd = NULL;
	char *opt_io = NULL;

	memset(&xw, 0, sizeof(xw));

	xw.default_title	= "st";
	xw.class		= TERMNAME;
	xw.fontname		= font;
	xw.term.setcolorname	= xsetcolorname;
	xw.term.settitle	= xsettitle;
	xw.term.seturgent	= xseturgency;

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
				xw.fontname = argv[i];
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
	term_init(&xw.term, 80, 24, shell, opt_cmd, opt_io, xw.win,
		  defaultfg, defaultbg, defaultcs);
	xinit(&xw);
	run(&xw);

	return 0;
}
