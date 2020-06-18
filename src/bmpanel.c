/*
 * Copyright (C) 2008 nsf
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <X11/Xutil.h>

/* composite */
#if defined(WITH_COMPOSITE)
 #include <X11/extensions/Xcomposite.h>
#endif

/* event loop */
#if defined(WITH_EV)
 #include <ev.h>
#elif defined(WITH_EVENT)
 #include <event.h>
#else
 #include <sys/timerfd.h>
#endif

#include "logger.h"
#include "theme.h"
#include "render.h"
#include "version.h"
#include "bmpanel.h"

/**************************************************************************
  GLOBALS
**************************************************************************/

struct mwmhints {
	uint32_t flags;
	uint32_t functions;
	uint32_t decorations;
	int32_t input_mode;
	uint32_t status;
};

static char *atom_names[] = {
	"WM_STATE",
	"_NET_DESKTOP_NAMES",
	"_NET_WM_STATE",
	"_NET_ACTIVE_WINDOW",
	"_NET_WM_NAME",
	"_NET_WM_ICON_NAME",
	"_NET_WM_VISIBLE_ICON_NAME",
	"_NET_WORKAREA",
	"_NET_WM_ICON",
	"_NET_WM_VISIBLE_NAME",
	"_NET_WM_STATE_SKIP_TASKBAR",
	"_NET_WM_STATE_SHADED",
	"_NET_WM_STATE_HIDDEN",
	"_NET_WM_DESKTOP",
	"_NET_MOVERESIZE_WINDOW",
	"_NET_WM_WINDOW_TYPE",
	"_NET_WM_WINDOW_TYPE_DOCK",
	"_NET_WM_WINDOW_TYPE_DESKTOP",
	"_NET_WM_STRUT",
	"_NET_WM_STRUT_PARTIAL",
	"_NET_CLIENT_LIST",
	"_NET_NUMBER_OF_DESKTOPS",
	"_NET_CURRENT_DESKTOP",
	"_NET_SYSTEM_TRAY_OPCODE",
	"UTF8_STRING",
	"_MOTIF_WM_HINTS",
	"_XROOTPMAP_ID"
};

#ifndef PREFIX
#define PREFIX "/usr"
#endif

#define HOME_THEME_PATH "/.config/bmpanel/themes"
#define SHARE_THEME_PATH PREFIX "/share/bmpanel/themes"

#define TRAY_REQUEST_DOCK 0
#define MWM_HINTS_DECORATIONS (1L << 1)

static struct xinfo X;
static struct panel P;

static int timerfd;

static int commence_taskbar_redraw;
static int commence_panel_redraw;
static int commence_switcher_redraw;

static const char *theme = "darkmini";
static const char *version = "bmpanel version " BMPANEL_VERSION;
static const char *usage = "usage: bmpanel [--version] [--help] [--usage] [--list] THEME";

static void cleanup();

/**************************************************************************
  X error handlers
**************************************************************************/

static int X_error_handler(Display *dpy, XErrorEvent *error)
{
	char buf[1024];
	if (error->error_code == BadWindow)
		return 0;
	XGetErrorText(dpy, error->error_code, buf, sizeof(buf));
	LOG_WARNING("X error: %s (resource id: %d)", buf, error->resourceid);
	return 0;
}

static int X_io_error_handler(Display *dpy)
{
	LOG_ERROR("fatal error, connection to X server lost? exiting...");
	return 0;
}

/**************************************************************************
  window properties
**************************************************************************/

static void *get_prop_data(Window win, Atom prop, Atom type, int *items)
{
	Atom type_ret;
	int format_ret;
	unsigned long items_ret;
	unsigned long after_ret;
	uchar *prop_data;

	prop_data = 0;

	XGetWindowProperty(X.display, win, prop, 0, 0x7fffffff, False,
			type, &type_ret, &format_ret, &items_ret,
			&after_ret, &prop_data);
	if (items)
		*items = items_ret;

	return prop_data;
}

static int get_prop_int(Window win, Atom at)
{
	int num = 0;
	long *data;

	data = get_prop_data(win, at, XA_CARDINAL, 0);
	if (data) {
		num = *data;
		XFree(data);
	}
	return num;
}

static Window get_prop_window(Window win, Atom at)
{
	Window num = 0;
	Window *data;

	data = get_prop_data(win, at, XA_WINDOW, 0);
	if (data) {
		num = *data;
		XFree(data);
	}
	return num;
}

static Pixmap get_prop_pixmap(Window win, Atom at)
{
	Pixmap num = 0;
	Pixmap *data;

	data = get_prop_data(win, at, XA_PIXMAP, 0);
	if (data) {
		num = *data;
		XFree(data);
	}
	return num;
}

static int get_window_desktop(Window win)
{
	return get_prop_int(win, X.atoms[XATOM_NET_WM_DESKTOP]);
}

static int is_window_hidden(Window win)
{
	Atom *data;
	int ret = 0;
	int num;

	data = get_prop_data(win, X.atoms[XATOM_NET_WM_WINDOW_TYPE], XA_ATOM, &num);
	if (data) {
		if (*data == X.atoms[XATOM_NET_WM_WINDOW_TYPE_DOCK] ||
		    *data == X.atoms[XATOM_NET_WM_WINDOW_TYPE_DESKTOP]) 
		{
			XFree(data);
			return 1;
		}
		XFree(data);
	}

	data = get_prop_data(win, X.atoms[XATOM_NET_WM_STATE], XA_ATOM, &num);
	if (!data)
		return 0;

	while (num) {
		num--;
		if (data[num] == X.atoms[XATOM_NET_WM_STATE_SKIP_TASKBAR])
			ret = 1;
	}
	XFree(data);

	return ret;
}

static int is_window_iconified(Window win)
{
	long *data;
	int ret = 0;

	data = get_prop_data(win, X.atoms[XATOM_WM_STATE], 
	     		X.atoms[XATOM_WM_STATE], 0);
	if (data) {
		if (data[0] == IconicState) {
			ret = 1;
		}
		XFree(data);
	}

	int num;
	data = get_prop_data(win, X.atoms[XATOM_NET_WM_STATE], XA_ATOM, &num);
	if (data) {
		while (num) {
			num--;
			if (data[num] == X.atoms[XATOM_NET_WM_STATE_HIDDEN])
				ret = 1;
		}
		XFree(data);
	}

	return ret;
}

static Imlib_Image get_window_icon(Window win)
{
	if (!THEME_USE_TASKBAR_ICON(P.theme))
		return 0;

	Imlib_Image ret = 0;

	int num = 0;
	long *data = get_prop_data(win, X.atoms[XATOM_NET_WM_ICON], 
					XA_CARDINAL, &num);
	if (data) {
		long *datal = data;
		uint32_t w,h;
		int i;
		w = *datal++;
		h = *datal++;
		/* hack for 64 bit systems */
		uint32_t *array = XMALLOC(uint32_t, w*h);
		for (i = 0; i < w*h; ++i) 
			array[i] = datal[i];
		ret = imlib_create_image_using_copied_data(w,h,array);
		xfree(array);
		imlib_context_set_image(ret);
		imlib_image_set_has_alpha(1);
		XFree(data);
	}

	if (!ret) {
	        XWMHints *hints = XGetWMHints(X.display, win);
		if (hints) {
			if (hints->flags & IconPixmapHint) {
				Pixmap pix;
				int x = 0, y = 0;
				uint w = 0, h = 0, d = 0, bw = 0;
				XGetGeometry(X.display, hints->icon_pixmap, &pix, 
						&x, &y, &w, &h, &bw, &d);
	
				imlib_context_set_drawable(hints->icon_pixmap);
				ret = imlib_create_image_from_drawable(hints->icon_mask, 
								x, y, w, h, 1);
			}
	        	XFree(hints);
		}
	}

	/* if we can't get icon, set default and return */
	if (!ret) {
		ret = P.theme->taskbar.default_icon_img;
		return ret;
	}

	/* well, we have our icon, lets resize it for faster rendering */
	int w,h;
	imlib_context_set_image(ret);
	w = imlib_image_get_width();
	h = imlib_image_get_height();
	Imlib_Image sizedicon = imlib_create_cropped_scaled_image(0, 0, w, h, 
			P.theme->taskbar.icon_w, P.theme->taskbar.icon_h);
	imlib_free_image();
	imlib_context_set_image(sizedicon);
	imlib_image_set_has_alpha(1);

	return sizedicon;
}

static char *alloc_window_name(Window win)
{
	char *ret, *name = 0;
	name = get_prop_data(win, X.atoms[XATOM_NET_WM_VISIBLE_ICON_NAME], X.atoms[XATOM_UTF8_STRING], 0);
	if (name) 
		goto name_here;
	name = get_prop_data(win, X.atoms[XATOM_NET_WM_ICON_NAME], X.atoms[XATOM_UTF8_STRING], 0);
	if (name) 
		goto name_here;
	name = get_prop_data(win, XA_WM_ICON_NAME, XA_STRING, 0);
	if (name) 
		goto name_here;
	name = get_prop_data(win, X.atoms[XATOM_NET_WM_VISIBLE_NAME], X.atoms[XATOM_UTF8_STRING], 0);
	if (name) 
		goto name_here;
	name = get_prop_data(win, X.atoms[XATOM_NET_WM_NAME], X.atoms[XATOM_UTF8_STRING], 0);
	if (name) 
		goto name_here;
	name = get_prop_data(win, XA_WM_NAME, XA_STRING, 0);
	if (name) 
		goto name_here;

	return xstrdup("<unknown>");
name_here:
	ret = xstrdup(name);
	XFree(name);
	return ret;
}

/**************************************************************************
  creating panel window
**************************************************************************/

#ifdef WITH_COMPOSITE
static Visual *find_argb_visual()
{
	XVisualInfo *xvi;
	XVisualInfo template;
	int nvi, i;
	XRenderPictFormat *format;
	Visual *visual;
	
	template.screen = X.screen;
	template.depth = 32;
	template.class = TrueColor;
	xvi = XGetVisualInfo(X.display,
			     VisualScreenMask |
			     VisualDepthMask |
			     VisualClassMask,
			     &template,
			     &nvi);
	if (xvi == 0)
		return 0;
	
	visual = 0;
	for (i = 0; i < nvi; i++) {
		format = XRenderFindVisualFormat(X.display, xvi[i].visual);
		if (format->type == PictTypeDirect && format->direct.alphaMask) {
			visual = xvi[i].visual;
			break;
		}
	}

	XFree(xvi);
	return visual;
}

static void setup_composite()
{
	int eventb, errorb;
	if (XCompositeQueryExtension(X.display, &eventb, &errorb) == False) {
		LOG_WARNING("composite extension isn't available on server, disabling composite");
		P.theme->use_composite = 0;
		return;
	}

	if (XRenderQueryExtension(X.display, &eventb, &errorb) == False) {
		LOG_WARNING("render extension isn't available on server, disabling composite");
		P.theme->use_composite = 0;
		return;
	}

	Visual *argbv = find_argb_visual();
	if (!argbv) {
		LOG_WARNING("argb visual not found, disabling composite");
		P.theme->use_composite = 0;
		return;
	}
	X.visual = argbv;
	X.colmap = XCreateColormap(X.display, X.root, X.visual, AllocNone);
	X.attrs.override_redirect = 0;
	X.attrs.background_pixel = 0x0;
	X.attrs.colormap = X.colmap;
	X.attrs.border_pixel = 0;
	X.amask = CWBackPixel | CWColormap | CWOverrideRedirect | CWBorderPixel;
	X.depth = 32;
}
#endif

static Window create_panel_window(uint placement, int alignment, int h, int w, int hover)
{
	Window win;
	if (!hover)
		hover = h;
	int y = 0;
	long strut[4] = {0,0,0,hover + X.screen_height - X.wa_h - X.wa_y};
	long tmp;
	int x = X.wa_x;

	if (placement == PLACE_TOP) {
		y = X.wa_y;
		strut[3] = 0;
		strut[2] = hover + X.wa_y;
	} else if (placement == PLACE_BOTTOM)
		y = X.wa_y + X.wa_h - h;
	
	/* set width and align the panel*/
	if (w) {
		if (w > X.wa_w)
			w = X.wa_w;
		x += (alignment == ALIGN_CENTER) ? (int)((X.wa_w - w)/2) : 
				(alignment == ALIGN_RIGHT) ? X.wa_w - w : 0;
	}

	P.x = x; P.y = y;
	win = XCreateWindow(X.display, X.root, x, y, w, h, 0, 
			X.depth, InputOutput, X.visual, X.amask, &X.attrs);

	XSelectInput(X.display, win, ButtonPressMask | ExposureMask | StructureNotifyMask);

	/* get our place on desktop */
	XChangeProperty(X.display, win, X.atoms[XATOM_NET_WM_STRUT], XA_CARDINAL, 32,
			PropModeReplace, (uchar*)&strut, 4);

	static const struct {
		int s, e;
	} where[] = {
		[PLACE_TOP] = {8, 9},
		[PLACE_BOTTOM] = {10, 11}
	};

	long strutp[12] = {strut[0], strut[1], strut[2], strut[3],};
	strutp[where[placement].s] = x;
	strutp[where[placement].e] = x+w;
	XChangeProperty(X.display, win, X.atoms[XATOM_NET_WM_STRUT_PARTIAL], XA_CARDINAL, 32,
			PropModeReplace, (uchar*)&strutp, 12);

	/* we want to be on all desktops */
	tmp = -1;
	XChangeProperty(X.display, win, X.atoms[XATOM_NET_WM_DESKTOP], XA_CARDINAL, 32,
			PropModeReplace, (uchar*)&tmp, 1);

	/* we're panel! */
	tmp = X.atoms[XATOM_NET_WM_WINDOW_TYPE_DOCK];
	XChangeProperty(X.display, win, X.atoms[XATOM_NET_WM_WINDOW_TYPE], XA_ATOM, 32,
			PropModeReplace, (uchar*)&tmp, 1);
	
	/* place window on it's position */
	XSizeHints size_hints;

	/* we need this for pekwm (other modern WMs should ignore them) */
	size_hints.x = x;
	size_hints.y = y;
	size_hints.width = w;
	size_hints.height = h;

	size_hints.flags = PPosition | PMaxSize | PMinSize;
	size_hints.min_width = size_hints.max_width = w;
	size_hints.min_height = size_hints.max_height = h;
	XSetWMNormalHints(X.display, win, &size_hints);

	XWMHints wm_hints;
	wm_hints.flags = InputHint | StateHint;
	wm_hints.initial_state = 1;
	wm_hints.input = 0;
	XSetWMHints(X.display, win, &wm_hints);

	/* set motif decoration hints */
	struct mwmhints mwm = {MWM_HINTS_DECORATIONS,0,0,0,0};
	XChangeProperty(X.display, win, X.atoms[XATOM_MOTIF_WM_HINTS], X.atoms[XATOM_MOTIF_WM_HINTS], 
			32, PropModeReplace, (uchar*)&mwm, sizeof(struct mwmhints) / 4);

	/* set classhint */
	XClassHint *ch;
	if (!(ch = XAllocClassHint()))
		LOG_ERROR("failed to allocate memory for class hints");
	ch->res_name = "panel";
	ch->res_class = "bmpanel";
	XSetClassHint(X.display, win, ch);
	XFree(ch);
	
	XMapWindow(X.display, win);
	XSync(X.display, 0);

	/* also send message to wm (fluxbox bug?) */
	{
		XClientMessageEvent cli;
		cli.type = ClientMessage;
		cli.window = win;
		cli.message_type = X.atoms[XATOM_NET_WM_DESKTOP];
		cli.format = 32;
		cli.data.l[0] = 0xFFFFFFFF;
		cli.data.l[1] = 0;
		cli.data.l[2] = 0;
		cli.data.l[3] = 0;
		cli.data.l[4] = 0;
		
		XSendEvent(X.display, X.root, False, SubstructureNotifyMask | 
				SubstructureRedirectMask, (XEvent*)&cli);
	}

	return win;
}


/**************************************************************************
  desktop management
**************************************************************************/

static int get_active_desktop()
{
	return get_prop_int(X.root, X.atoms[XATOM_NET_CURRENT_DESKTOP]);
}

static void set_active_desktop(int d)
{
	int i = 0;
	struct desktop *iter = P.desktops;
	while (iter) {
		iter->focused = (i == d);
		iter = iter->next;
		i++;
	}
}

static int get_number_of_desktops()
{
	return get_prop_int(X.root, X.atoms[XATOM_NET_NUMBER_OF_DESKTOPS]);
}

static void free_desktops()
{
	struct desktop *iter, *next;
	iter = P.desktops;
	while (iter) {
		next = iter->next;
		xfree(iter->name);
		xfree(iter);
		iter = next;
	}
	P.desktops = 0;
}

static void rebuild_desktops()
{
	/* 
	 * This function is not optimal. It frees all the desktops and create them again 
	 * Anyway, if you change number of your desktops or desktop names in real time, you are
	 * probably using wrong software, or maybe you were born in wrong world. 
	 */
	free_desktops();

	struct desktop *last = P.desktops, *d = 0;
	int desktopsnum = get_number_of_desktops();
	int activedesktop = get_active_desktop();
	int i, len;

	char *name, *names;
	names = name = get_prop_data(X.root, X.atoms[XATOM_NET_DESKTOP_NAMES], 
			X.atoms[XATOM_UTF8_STRING], 0);

	for (i = 0; i < desktopsnum; ++i) {
		d = XMALLOCZ(struct desktop, 1);
		if (names)
			d->name = xstrdup(name);
		else {
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", i+1);
			d->name = xstrdup(buf);
		}
		d->focused = (i == activedesktop);
		if (!last) {
			P.desktops = d;
			last = d;
		} else {
			last->next = d;
			last = d;
		}

		if (names) {
			len = strlen(name);
			name += len + 1;
		}
	}

	if (names)
		XFree(names);
}

static void switch_desktop(int d)
{
	XClientMessageEvent e;

	if (d >= get_number_of_desktops())
		return;

	e.type = ClientMessage;
	e.window = X.root;
	e.message_type = X.atoms[XATOM_NET_CURRENT_DESKTOP];
	e.format = 32;
	e.data.l[0] = d;
	e.data.l[1] = 0;
	e.data.l[2] = 0;
	e.data.l[3] = 0;
	e.data.l[4] = 0;
	
	XSendEvent(X.display, X.root, False, SubstructureNotifyMask | 
			SubstructureRedirectMask, (XEvent*)&e);
}


/**************************************************************************
  task management
**************************************************************************/

static void activate_task(struct task *t)
{
	XClientMessageEvent e;

	e.type = ClientMessage;
	e.window = t ? t->win : None;
	e.message_type = X.atoms[XATOM_NET_ACTIVE_WINDOW];
	e.format = 32;
	e.data.l[0] = 2;
	e.data.l[1] = CurrentTime;
	e.data.l[2] = 0;
	e.data.l[3] = 0;
	e.data.l[4] = 0;

	XSendEvent(X.display, X.root, False, SubstructureNotifyMask |
			SubstructureRedirectMask, (XEvent*)&e);
}

static void free_tasks()
{
	struct task *iter, *next;
	iter = P.tasks;
	while (iter) {
		next = iter->next;
		if (iter->icon && iter->icon != P.theme->taskbar.default_icon_img) {
			imlib_context_set_image(iter->icon);
			imlib_free_image();
		}
		xfree(iter->name);
		xfree(iter);
		iter = next;
	}
}

static void add_task(Window win, uint focused)
{
	if (is_window_hidden(win))
		return;

	struct task *t = XMALLOCZ(struct task, 1);
	t->win = win;
	t->name = alloc_window_name(win); 
	t->desktop = get_window_desktop(win);
	t->iconified = is_window_iconified(win); 
	t->focused = focused;
	t->icon = get_window_icon(win);

	XSelectInput(X.display, win, PropertyChangeMask | 
			FocusChangeMask | StructureNotifyMask);

	/* put task in list */
	struct task *iter = P.tasks;
	if (!iter || iter->desktop > t->desktop) {
		t->next = P.tasks;
		P.tasks = t;
		return;
	}

	for (;;) {
		if (!iter->next || iter->next->desktop > t->desktop) {
			t->next = iter->next;
			iter->next = t;
			return;
		}
		iter = iter->next;
	}
}

static void sort_move_task(struct task *rt)
{
	struct task *prev = 0, *next, *iter, *t = P.tasks;
	while (t) {
		next = t->next;
		if (t->win == rt->win) {
			if (!prev)
				P.tasks = next;
			else 
				prev->next = next;
			break;
		}
		prev = t;
		t = next;
	}

	iter = P.tasks;
	if (!iter || iter->desktop > t->desktop) {
		t->next = P.tasks;
		P.tasks = t;
		return;
	}

	for (;;) {
		if (!iter->next || iter->next->desktop > t->desktop) {
			t->next = iter->next;
			iter->next = t;
			return;
		}
		iter = iter->next;
	}
}

static void del_task(Window win)
{
	struct task *prev = 0, *next, *iter = P.tasks;
	while (iter) {
		next = iter->next;
		if (iter->win == win) {
			if (iter->icon && iter->icon != P.theme->taskbar.default_icon_img) {
				imlib_context_set_image(iter->icon);
				imlib_free_image();
			}
			xfree(iter->name);
			xfree(iter);
			if (!prev)
				P.tasks = next;
			else
				prev->next = next;
			return;
		}
		prev = iter;
		iter = next;
	}
}

static struct task *find_task(Window win)
{
	struct task *iter = P.tasks;
	while (iter) {
		if (iter->win == win)
			return iter;
		iter = iter->next;
	}
	return 0;
}

static void update_tasks_focus(Window win)
{
	struct task *iter = P.tasks;
	while (iter) {
		iter->focused = (iter->win == win);
		iter = iter->next;
	}
}

static void update_tasks()
{
	Window *wins, focuswin;
	int num, i, j, rev;

	XGetInputFocus(X.display, &focuswin, &rev);

	wins = get_prop_data(X.root, X.atoms[XATOM_NET_CLIENT_LIST], XA_WINDOW, &num);

	/* if there are no client list? we are in not NETWM compliant wm? */
	/* if (!wins) return; */

	/* if one or more windows in my list are not in _NET_CLIENT_LIST, delete them */
	struct task *next, *iter = P.tasks;
	while (iter) {
		iter->focused = (focuswin == iter->win);
		next = iter->next;
		for (j = 0; j < num; ++j) {
			if (iter->win == wins[j])
				goto nodelete;
		}
		del_task(iter->win);
nodelete:
		iter = next;
	}

	/* for each window in _NET_CLIENT_LIST, check if it is in out list, if
	   it's not, add it */
	for (i = 0; i < num; ++i) {
		/* skip panel */
		if (wins[i] == P.win)
			continue;

		if (!find_task(wins[i]))
			add_task(wins[i], (wins[i] == focuswin));
	}
	XFree(wins);
}

/**************************************************************************
  systray functions
**************************************************************************/

/**************************************************************************
  X message handlers
**************************************************************************/

static void handle_property_notify(Window win, Atom a)
{
	/* global changes */
	if (win == X.root) {
		/* user or WM reconfigured it's desktops */
		if (a == X.atoms[XATOM_NET_NUMBER_OF_DESKTOPS] ||
		    a == X.atoms[XATOM_NET_DESKTOP_NAMES])
		{
			rebuild_desktops();
			render_update_panel_positions(&P);
			commence_panel_redraw = 1;
			return;
		}

		/* user or WM switched desktop */
		if (a == X.atoms[XATOM_NET_CURRENT_DESKTOP]) {
			set_active_desktop(get_active_desktop());
			render_update_panel_positions(&P);
			commence_switcher_redraw = 1;
			commence_taskbar_redraw = 1;
			return;
		}

		/* updates in client list */
		if (a == X.atoms[XATOM_NET_CLIENT_LIST]) {
			update_tasks();
			render_update_panel_positions(&P);
			commence_taskbar_redraw = 1;
			return;
		}

		if (a == X.atoms[XATOM_NET_ACTIVE_WINDOW]) {
			Window win = get_prop_window(X.root, X.atoms[XATOM_NET_ACTIVE_WINDOW]);
			update_tasks_focus(win);
			commence_taskbar_redraw = 1;
			return;
		}

		if (a == X.atoms[XATOM_XROOTPMAP_ID]) {
			X.rootpmap = get_prop_pixmap(X.root, X.atoms[XATOM_XROOTPMAP_ID]);
			render_update_panel_positions(&P);
			return;
		}
	}

	/* now it's time for per-window changes */
	struct task *t = find_task(win);
	if (!t) {
		render_present();
		return;
	}

	/* widow changed it's desktop */
	if (a == X.atoms[XATOM_NET_WM_DESKTOP]) {
		t->desktop = get_window_desktop(win);
		sort_move_task(t);
		render_update_panel_positions(&P);
		commence_switcher_redraw = 1;
		commence_taskbar_redraw = 1;
		return;
	}
	
	/* window changed it's visible name or name */
	if (a == X.atoms[XATOM_NET_WM_NAME] || 
	    a == X.atoms[XATOM_NET_WM_VISIBLE_NAME]) 
	{
		xfree(t->name);
		t->name = alloc_window_name(t->win);
		commence_taskbar_redraw = 1;
		return;
	}

	if (a == X.atoms[XATOM_NET_WM_STATE] ||
	    a == X.atoms[XATOM_WM_STATE]) 
	{
		if (is_window_hidden(t->win)) {
			del_task(t->win);
			return;
		}
		t->iconified = is_window_iconified(t->win);
		t->focused = (get_prop_window(X.root, X.atoms[XATOM_NET_ACTIVE_WINDOW]) == t->win);
		
		commence_taskbar_redraw = 1;
		return;
	}

	if (a == X.atoms[XATOM_NET_WM_ICON] ||
	    a == XA_WM_HINTS) 
	{
		if (t->icon && t->icon != P.theme->taskbar.default_icon_img) {
			imlib_context_set_image(t->icon);
			imlib_free_image();
		}
		t->icon = get_window_icon(t->win);
		commence_taskbar_redraw = 1;
		return;
	}
}

static void handle_button(int x, int y, int button)
{
	int adesk = get_active_desktop();

	/* second button iconize all windows, we want to see our desktop */
	if (button == 3) {
		struct task *iter = P.tasks;
		while (iter) {
			if (iter->desktop == adesk || iter->desktop == -1) {
				iter->iconified = 1;
				iter->focused = 0;
				XIconifyWindow(X.display, iter->win, X.screen);
			}
			iter = iter->next;
		}
		commence_taskbar_redraw = 1;
		return;
	}

	/* check desktops */
	int desk = 0;
	struct desktop *diter = P.desktops;
	while (diter) {
		if (x > diter->posx && x < diter->posx + diter->width && !diter->focused) {
			if (desk != adesk)
				switch_desktop(desk);
			/* redraw will be in property notify */
			break;
		}
		diter = diter->next;
		desk++;
	}

	/* check taskbar */
	struct task *iter = P.tasks;
	while (iter) {
		if ((iter->desktop == adesk || iter->desktop == -1) &&
		    x > iter->posx && 
		    x < iter->posx + iter->width) 
		{
			if (iter->iconified) {
				iter->iconified = 0;
				iter->focused = 1;
				activate_task(iter);
			} else {
				if (iter->focused) {
					iter->iconified = 1;
					iter->focused = 0;
					XIconifyWindow(X.display, iter->win, X.screen);
				} else {
					iter->focused = 1;
					activate_task(iter);
					
					XWindowChanges wc;
					wc.stack_mode = Above;
					XConfigureWindow(X.display, iter->win, CWStackMode, &wc);
				}
			}
			/* commence_taskbar_redraw = 1; */
		} else if (iter->desktop == adesk) {
			iter->focused = 0;
		}
		iter = iter->next;
	}
}

static void handle_focusin(Window win)
{
	struct task *iter = P.tasks;
	while (iter) {
		iter->focused = (iter->win == win);
		iter = iter->next;
	}
}

/**************************************************************************
  initialization
**************************************************************************/

static void initX()
{
	/* open connection to X server */
	X.display = XOpenDisplay(0);
	if (!X.display)
		LOG_ERROR("failed connect to X server");
	XSetErrorHandler(X_error_handler);
	XSetIOErrorHandler(X_io_error_handler);
	
	memset(&X.attrs, 0, sizeof(X.attrs));

	/* useful variables */
	X.screen 	= DefaultScreen(X.display);
	X.screen_width 	= DisplayWidth(X.display, X.screen);
	X.screen_height	= DisplayHeight(X.display, X.screen);
	X.visual 	= DefaultVisual(X.display, X.screen);
	X.colmap	= CopyFromParent;
	X.root 		= RootWindow(X.display, X.screen);
	X.amask		= 0;
	X.depth 	= DefaultDepth(X.display, X.screen);
	X.wa_x 		= 0;
	X.wa_y 		= 0;
	X.wa_w 		= X.screen_width;
	X.wa_h 		= X.screen_height;
	
	/* get internal atoms */
	XInternAtoms(X.display, atom_names, XATOM_COUNT, False, X.atoms);
	XSelectInput(X.display, X.root, PropertyChangeMask);

	X.rootpmap = get_prop_pixmap(X.root, X.atoms[XATOM_XROOTPMAP_ID]);

	/* get workarea */
	long *workarea = get_prop_data(X.root, X.atoms[XATOM_NET_WORKAREA], XA_CARDINAL, 0);
	if (workarea) {
		X.wa_x = workarea[0];
		X.wa_y = workarea[1];
		X.wa_w = workarea[2];
		X.wa_h = workarea[3];
		XFree(workarea);	
	}
}

static void initP(const char *theme)
{
	char dirbuf[4096];
	/* first try to find theme in user home dir */
	snprintf(dirbuf, sizeof(dirbuf), "%s%s/%s", 
			getenv("HOME"), 
			HOME_THEME_PATH,
			theme);
	P.theme = load_theme(dirbuf);
	if (P.theme) goto validation;

	/* now try share dir */
	snprintf(dirbuf, sizeof(dirbuf), "%s/%s",
			SHARE_THEME_PATH,
			theme);
	P.theme = load_theme(dirbuf);
	if (P.theme) goto validation;

	/* and last try is absolute or relative dir */
	P.theme = load_theme(theme);
	if (!P.theme)
		LOG_ERROR("failed to load theme: %s", theme);

validation:
	/* validate theme */
	if (!theme_is_valid(P.theme))
		LOG_ERROR("invalid theme: %s", theme);

	/* setup composite if necessary */
#ifdef WITH_COMPOSITE
	if (P.theme->use_composite)
		setup_composite();
#endif

	/* create panel window */
	P.width = X.wa_w;
	if (P.theme->width)
		P.width = (P.theme->width_type == WIDTH_TYPE_PERCENT) ? 
			(int)((X.wa_w * P.theme->width) / 100) : 
			P.theme->width;
	P.win = create_panel_window(P.theme->placement, 
			            P.theme->alignment, 
				    P.theme->height,
				    P.width,
				    P.theme->height_override);

#ifdef WITH_COMPOSITE
	if (P.theme->use_composite)
		XCompositeRedirectSubwindows(X.display, P.win, CompositeRedirectAutomatic);

	if (P.theme->use_composite && is_element_in_theme(P.theme, 't')) {
		LOG_WARNING("tray cannot be used with composite mode enabled");
		theme_remove_element(P.theme, 't');
	}
#endif

}

/**************************************************************************
  cleanup
**************************************************************************/

static void freeP()
{
	free_theme(P.theme);
	free_tasks();
	free_desktops();
	XDestroyWindow(X.display, P.win);
	XCloseDisplay(X.display);
}

static void cleanup()
{
	shutdown_render();
	freeP();
	/* close(timerfd); */
	LOG_MESSAGE("cleanup");
}

/**************************************************************************
  event callbacks
**************************************************************************/

static void xconnection_cb()
{
	XEvent e;
	while (XPending(X.display)) {
		XNextEvent(X.display, &e);
		switch (e.type) {
		case Expose:
			commence_panel_redraw = 1;
			break;
		case ButtonPress:
			handle_button(e.xbutton.x, e.xbutton.y, e.xbutton.button);
			break;
		case PropertyNotify:
			handle_property_notify(e.xproperty.window, e.xproperty.atom);	
			break;
		case FocusIn:
			handle_focusin(e.xfocus.window);
			render_update_panel_positions(&P);
			commence_taskbar_redraw = 1;
			break;
		default:
			break;
		}
		XSync(X.display, 0);
		
		if (commence_panel_redraw) {
			render_panel(&P);
		} else if (commence_switcher_redraw || commence_taskbar_redraw) {
			if (commence_switcher_redraw) {
				render_switcher(P.desktops);
			}
			if (commence_taskbar_redraw) {
				render_taskbar(P.tasks, P.desktops);
			}
			render_present();
		}
		commence_panel_redraw = 0;
		commence_switcher_redraw = 0;
		commence_taskbar_redraw = 0;
	}
}


/**************************************************************************
  signal handlers
**************************************************************************/

static void sighup_handler(int xxx)
{
	LOG_MESSAGE("sighup signal received");
	cleanup();
	xmemleaks();
	exit(0);
}

static void sigint_handler(int xxx)
{
	LOG_MESSAGE("sigint signal received");
	cleanup();
	xmemleaks();
	exit(0);
}

/**************************************************************************
  list themes
**************************************************************************/

static void list_themes_in_dir(DIR *d)
{
	struct dirent *de;
	int len;

	while ((de = readdir(d)) != 0) {
		len = strlen(de->d_name);
		switch (len) {
		case 1:
			if (de->d_name[0] == '.')
				continue;
		case 2:
			if (de->d_name[0] == '.' && de->d_name[1] == '.')
				continue;
		default:
			break;
		}
		LOG_MESSAGE("* %s", de->d_name);
	}
}

static void list_themes()
{
	char dirbuf[4096];
	DIR *d;

	snprintf(dirbuf, sizeof(dirbuf), "%s%s", 
			getenv("HOME"), 
			HOME_THEME_PATH);
	LOG_MESSAGE("user themes in %s", dirbuf);
	d = opendir(dirbuf);
	if (d) {
		list_themes_in_dir(d);
		closedir(d);
	} else
		LOG_MESSAGE("- none");

	LOG_MESSAGE("system themes in %s", SHARE_THEME_PATH);
	d = opendir(SHARE_THEME_PATH);
	if (d) {
		list_themes_in_dir(d);
		closedir(d);
	} else
		LOG_MESSAGE("- none");
}

/**************************************************************************
  main event loop
**************************************************************************/

#if defined(WITH_EV)
/* ---------- libev implementation ---------- */
static void xconnection_cb_ev(EV_P_ struct ev_io *w, int revents)
{
	xconnection_cb();
}

static void init_and_start_loop()
{
	int xfd = ConnectionNumber(X.display);
	struct ev_loop *el = ev_default_loop(0);
	ev_timer clock_redraw;
	ev_io xconnection;

	/* macros?! whuut?! */
	xconnection.active = xconnection.pending = xconnection.priority = 0;
	xconnection.cb = xconnection_cb_ev;
	xconnection.fd = xfd; 
	xconnection.events = EV_READ | EV_IOFDSET;

	clock_redraw.active = clock_redraw.pending = clock_redraw.priority = 0;
	clock_redraw.at = clock_redraw.repeat = 1.0f;

	ev_io_start(el, &xconnection);
	ev_timer_start(el, &clock_redraw);
	ev_loop(el, 0);
}
#elif defined(WITH_EVENT)
/* ---------- libevent implementation ---------- */
static void xconnection_cb_event(int fd, short type, void *arg)
{
	xconnection_cb();
	
	/* reschedule */
	event_add((struct event*)arg, 0);
}

static void init_and_start_loop()
{
	int xfd = ConnectionNumber(X.display);
	struct event clock_redraw;
	struct event xconnection;
	struct timeval tv = {1, 0};

	event_init();
	event_add(&clock_redraw, &tv); 

	event_set(&xconnection, xfd, EV_READ, xconnection_cb_event, &xconnection);
	event_add(&xconnection, 0);

	event_dispatch();
}
#else
/* ---------- glibc 2.8 + timerfd in linux kernel ---------- */
static void init_and_start_loop()
{
	fd_set events;
	int maxfd;
	int xfd;

	/* get connection fd from Xlib */
	xfd = ConnectionNumber(X.display);

	/* create timer fd to deal with timer and connection in one thread */
	timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (timerfd == -1)
		LOG_ERROR("failed to create timer fd");
	fcntl(timerfd, F_SETFL, O_NONBLOCK);

	maxfd = (timerfd > xfd) ? timerfd : xfd;

	/* 1 second interval */
	struct itimerspec tspec = {{1,0},{1,0}};
	timerfd_settime(timerfd, 0, &tspec, 0);

	while (1) {
		FD_ZERO(&events);
		FD_SET(xfd, &events);
		FD_SET(timerfd, &events);

		if (select(maxfd+1, &events, 0, 0, 0) == -1)
			break;

		if (FD_ISSET(xfd, &events)) 
			xconnection_cb();
		if (FD_ISSET(timerfd, &events)) {
			uint64_t tmp = 0;
			/* dump all stuff from timer fd to nowhere */
			while (read(timerfd, &tmp, sizeof(uint64_t)) > 0)
				/* do nothing */;
		}
	}
}
#endif

static void parse_args(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--version")) {
			LOG_MESSAGE(version);
			exit(0);
		}
		if (!strcmp(arg, "--help")) {
			LOG_MESSAGE("There are no help, no manpage yet. See INSTALL "
				"in source distro and/or visit:\nhttp://nsf.110mb.com/bmpanel");
			exit(0);
		}
		if (!strcmp(arg, "--usage")) {
			LOG_MESSAGE(usage);
			exit(0);
		}
		if (!strcmp(arg, "--list")) {
			list_themes();
			exit(0);
		}
		break;
	}

	if (i < argc) {
		theme = argv[i];
	}
}

int main(int argc, char **argv)
{
	log_attach_callback(log_console_callback);
	parse_args(argc, argv);
	LOG_MESSAGE("starting bmpanel with theme: %s", theme);

	initX();
	initP(theme);
	init_render(&X, &P);

	signal(SIGHUP, sighup_handler);
	signal(SIGINT, sigint_handler);

	rebuild_desktops();
	update_tasks();

	render_update_panel_positions(&P);
	render_panel(&P);

	XSync(X.display, 0);
	init_and_start_loop();

	cleanup();
	return 0;
}

