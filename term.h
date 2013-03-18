#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

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

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Other macros */

#define SPACES_PER_TAB	8

/* TERM value */
#define TERMNAME	"st-256color"

#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))

#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ

#define VT102ID "\033[?6c"

enum escape_state {
	ESC_START = 1,
	ESC_CSI = 2,
	ESC_STR = 4,		/* DSC, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END = 16,	/* a final string was encountered */
	ESC_TEST = 32,		/* Enter in test mode */
};

struct st_glyph {
	unsigned	c;		/* character code */
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

struct st_selection {
	enum {
		SEL_NONE,
		SEL_REGULAR,
		SEL_RECTANGULAR,
	}		type;

	struct coord	start, end;
	struct coord	p1, p2;

	char		*clip;
};

/* Internal representation of the screen */
struct st_term {
	int		cmdfd;
	unsigned char	cmdbuf[BUFSIZ];
	unsigned	cmdbuflen;

	int		logfd;
	const char	*logfile;

	struct coord	size;
	struct coord	ttysize; /* kill? */
	struct st_glyph	**line;	/* screen */
	struct st_glyph	**alt;	/* alternate screen */
	bool		*dirty;	/* dirtyness of lines */
	bool		*tabs;

	struct tcursor	c;	/* cursor */
	struct tcursor	saved;
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

	unsigned short	defaultfg;
	unsigned short	defaultbg;
	unsigned short	defaultcs;
	unsigned short	defaultucs;

	int		(*setcolorname)(struct st_term *, int, const char *);
	void		(*settitle)(struct st_term *, char *);
	void		(*seturgent)(struct st_term *, int);
};

bool term_selected(struct st_selection *sel, int x, int y);
void term_sel_copy(struct st_term *term);
void term_sel_start(struct st_term *term, unsigned type, struct coord start);
void term_sel_end(struct st_term *term, struct coord end);

void term_echo(struct st_term *term, char *buf, int len);
void term_read(struct st_term *term);

void term_resize(struct st_term *term, struct coord size);
void term_shutdown(struct st_term *term);
void term_init(struct st_term *term, int col, int row, char *shell,
	       char **cmd, const char *logfile, unsigned long windowid,
	       unsigned defaultfg, unsigned defaultbg, unsigned defaultcs,
	       unsigned defaultucs);

/* Random utility code */

static inline void die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static inline void edie(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	fprintf(stderr, ": %s\n", strerror(errno));
	exit(EXIT_FAILURE);
}

static inline ssize_t xwrite(int fd, void *s, size_t len)
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

static inline void *xmalloc(size_t len)
{
	void *p = malloc(len);

	if (!p)
		die("Out of memory\n");

	return p;
}

static inline void *xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("Out of memory\n");

	return p;
}

static inline void *xcalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);

	if (!p)
		die("Out of memory\n");

	return p;
}

static inline void ttywrite(struct st_term *term, const char *s, size_t n)
{
	if (write(term->cmdfd, s, n) == -1)
		die("write error on tty: %s\n", strerror(errno));
}

static inline struct st_glyph *term_pos(struct st_term *term, struct coord pos)
{
	return &term->line[pos.y][pos.x];
}

static inline void tsetdirt(struct st_term *term, unsigned top, unsigned bot)
{
	bot = min(bot, term->size.y - 1);

	for (unsigned i = top; i <= bot; i++)
		term->dirty[i] = 1;
}

static inline void tfulldirt(struct st_term *term)
{
	tsetdirt(term, 0, term->size.y - 1);
}
