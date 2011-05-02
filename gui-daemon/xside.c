/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <execinfo.h>
#include <assert.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/Xatom.h>
#include "messages.h"
#include "txrx.h"
#include "list.h"
#include "error.h"
#include "shm_cmd.h"
#include "cmd_socket.h"
#include "qlimits.h"
#include "tray.h"

struct _global_handles {
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	int root_width;
	int root_height;
	GC context;
	Atom wmDeleteMessage;
	Atom tray_selection;	/* Atom: _NET_SYSTEM_TRAY_SELECTION_S<creen number> */
	Atom tray_opcode;	/* Atom: _NET_SYSTEM_TRAY_MESSAGE_OPCODE */
	Atom xembed_message;	/* Atom: _XEMBED */
	char vmname[16];
	struct shm_cmd *shmcmd;
	int cmd_shmid;
	int domid;
	int execute_cmd_in_vm;
	char cmd_for_vm[256];
	int clipboard_requested;
	GC frame_gc;
	GC tray_gc;
	char *cmdline_color;
	char *cmdline_icon;
	int label_index;
};

typedef struct _global_handles Ghandles;
Ghandles ghandles;
struct conndata {
	int width;
	int height;
	int x;
	int y;
	int is_mapped;
	int is_docked;
	XID remote_winid;
	Window local_winid;
	struct conndata *parent;
	struct conndata *transient_for;
	int override_redirect;
	XShmSegmentInfo shminfo;
	XImage *image;
	int image_height;
	int image_width;
	int have_queued_configure;
};
struct conndata *last_input_window;
struct genlist *remote2local;
struct genlist *wid2conndata;

#define BORDER_WIDTH 2

#define VERIFY(x) if (!(x)) { \
		fprintf(stderr, \
			"%s:%d: Received values doesn't pass verification: %s\nAborting\n", \
				__FILE__, __LINE__, __STRING(x)); \
		exit(1); \
	}
#ifndef min
#define min(x,y) ((x)>(y)?(y):(x))
#endif
#ifndef max
#define max(x,y) ((x)<(y)?(y):(x))
#endif

Window mkwindow(Ghandles * g, struct conndata *item)
{
	char *gargv[1] = { 0 };
	Window child_win;
	Window parent;
	XSizeHints my_size_hints;	/* hints for the window manager */
	Atom atom_label;

	my_size_hints.flags = PSize;
	my_size_hints.height = item->width;
	my_size_hints.width = item->height;

	if (item->parent)
		parent = item->parent->local_winid;
	else
		parent = g->root_win;
#if 0
	if (item->override_redirect && !item->parent && last_input_window) {
		XWindowAttributes attr;
		ret =
		    XGetWindowAttributes(g->display,
					 last_input_window->local_winid,
					 &attr);
		if (ret != 0) {
			parent = last_input_window->local_winid;
			x = item->remote_x - last_input_window->remote_x +
			    attr.x;
			y = item->remote_y - last_input_window->remote_y +
			    attr.y;
		}
	}
#endif
#if 1
	// we will set override_redirect later, if needed
	child_win = XCreateSimpleWindow(g->display, parent,
					item->x, item->y, item->width,
					item->height, 0,
					BlackPixel(g->display, g->screen),
					WhitePixel(g->display, g->screen));
#endif
#if 0
	attr.override_redirect = item->override_redirect;
	child_win = XCreateWindow(g->display, parent,
				  x, y, item->width, item->height,
				  0,
				  CopyFromParent,
				  CopyFromParent,
				  CopyFromParent,
				  CWOverrideRedirect, &attr);
#endif
/* pass my size hints to the window manager, along with window
	and icon names */
	(void) XSetStandardProperties(g->display, child_win,
				      "VMapp command", "Pixmap", None,
				      gargv, 0, &my_size_hints);
	(void) XSelectInput(g->display, child_win,
			    ExposureMask | KeyPressMask | KeyReleaseMask |
			    ButtonPressMask | ButtonReleaseMask |
			    PointerMotionMask | EnterWindowMask |
			    FocusChangeMask | StructureNotifyMask);
	XSetWMProtocols(g->display, child_win, &g->wmDeleteMessage, 1);
	if (g->cmdline_icon) {
		XClassHint class_hint =
		    { g->cmdline_icon, g->cmdline_icon };
		XSetClassHint(g->display, child_win, &class_hint);
	}
	// Set '_QUBES_LABEL' property so that Window Manager can read it and draw proper decoration
	atom_label = XInternAtom(g->display, "_QUBES_LABEL", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_CARDINAL,
			8 /* 8 bit is enough */ , PropModeReplace,
			(unsigned char *) &g->label_index, 1);

	// Set '_QUBES_VMNAME' property so that Window Manager can read it and nicely display it
	atom_label = XInternAtom(g->display, "_QUBES_VMNAME", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_STRING,
			8 /* 8 bit is enough */ , PropModeReplace,
			(const unsigned char *) g->vmname,
			strlen(g->vmname));


	return child_win;
}


#define XORG_DEFAULT_XINC 8
#define _VIRTUALX(x) ( (((x)+XORG_DEFAULT_XINC-1)/XORG_DEFAULT_XINC)*XORG_DEFAULT_XINC )
void mkghandles(Ghandles * g)
{
	char tray_sel_atom_name[64];
	XWindowAttributes attr;
	g->display = XOpenDisplay(NULL);
	if (!g->display) {
		perror("XOpenDisplay");
		exit(1);
	}
	g->screen = DefaultScreen(g->display);
	g->root_win = RootWindow(g->display, g->screen);
	XGetWindowAttributes(g->display, g->root_win, &attr);
	g->root_width = _VIRTUALX(attr.width);
	g->root_height = attr.height;
	g->context = XCreateGC(g->display, g->root_win, 0, NULL);
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", True);
	g->clipboard_requested = 0;
	snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
		 "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display));
	g->tray_selection =
	    XInternAtom(g->display, tray_sel_atom_name, False);
	g->tray_opcode =
	    XInternAtom(g->display, "_NET_SYSTEM_TRAY_OPCODE", False);
	g->xembed_message = XInternAtom(g->display, "_XEMBED", False);
}

struct conndata *check_nonmanged_window(XID id)
{
	struct genlist *item = list_lookup(wid2conndata, id);
	if (!item) {
		fprintf(stderr, "cannot lookup 0x%x in wid2conndata\n",
			(int) id);
		return 0;
	}
	return item->data;
}

void inter_appviewer_lock(int mode);

#define CHECK_NONMANAGED_WINDOW(id) struct conndata *conn; \
	if (!(conn=check_nonmanged_window(id))) return

#define QUBES_CLIPBOARD_FILENAME "/var/run/qubes/qubes_clipboard.bin"
void get_qubes_clipboard(char **data, int *len)
{
	FILE *file;
	*len = 0;
	inter_appviewer_lock(1);
	file = fopen(QUBES_CLIPBOARD_FILENAME, "r");
	if (!file)
		goto out;
	fseek(file, 0, SEEK_END);
	*len = ftell(file);
	*data = malloc(*len);
	if (!*data) {
		perror("malloc");
		exit(1);
	}
	fseek(file, 0, SEEK_SET);
	fread(*data, *len, 1, file);
	fclose(file);
	truncate(QUBES_CLIPBOARD_FILENAME, 0);
	file = fopen(QUBES_CLIPBOARD_FILENAME ".source", "w+");
	fclose(file);
out:
	inter_appviewer_lock(0);
}

void handle_clipboard_data(unsigned int untrusted_len)
{
	FILE *file;
	char *untrusted_data;
	size_t untrusted_data_sz;
	fprintf(stderr, "handle_clipboard_data, len=0x%x\n", untrusted_len);
	if (untrusted_len > MAX_CLIPBOARD_SIZE) {
		fprintf(stderr, "clipboard data len 0x%x?\n", untrusted_len);
		exit(1);
	}
	/* now sanitized */
	untrusted_data_sz = untrusted_len;
	untrusted_data = malloc(untrusted_data_sz);
	if (!untrusted_data) {
		perror("malloc");
		exit(1);
	}
	read_data(untrusted_data, untrusted_data_sz);
	if (!ghandles.clipboard_requested) {
		free(untrusted_data);
		fprintf(stderr, "received clipboard data when not requested\n");
		return;
	}
	inter_appviewer_lock(1);
	file = fopen(QUBES_CLIPBOARD_FILENAME, "w");
	if (!file) {
		perror("open " QUBES_CLIPBOARD_FILENAME);
		exit(1);
	}
	fwrite(untrusted_data, untrusted_data_sz, 1, file);
	fclose(file);
	file = fopen(QUBES_CLIPBOARD_FILENAME ".source", "w");
	if (!file) {
		perror("open " QUBES_CLIPBOARD_FILENAME ".source");
		exit(1);
	}
	fwrite(ghandles.vmname, strlen(ghandles.vmname), 1, file);
	fclose(file);
	inter_appviewer_lock(0);
	ghandles.clipboard_requested = 0;
	free(untrusted_data);
}

int is_special_keypress(XKeyEvent * ev, XID remote_winid)
{
	struct msghdr hdr;
	char *data;
	int len;
	if ((ev->state & (ShiftMask | ControlMask)) !=
	    (ShiftMask | ControlMask))
		return 0;
	if (ev->keycode == XKeysymToKeycode(ghandles.display, XK_c)) {
		if (ev->type != KeyPress)
			return 1;
		ghandles.clipboard_requested = 1;
		hdr.type = MSG_CLIPBOARD_REQ;
		hdr.window = remote_winid;
		fprintf(stderr, "Ctrl-Shift-c\n");
		write_struct(hdr);
		return 1;
	}
	if (ev->keycode == XKeysymToKeycode(ghandles.display, XK_v)) {
		if (ev->type != KeyPress)
			return 1;
		hdr.type = MSG_CLIPBOARD_DATA;
		fprintf(stderr, "Ctrl-Shift-v\n");
		get_qubes_clipboard(&data, &len);
		if (len > 0) {
			hdr.window = len;
			real_write_message((char *) &hdr, sizeof(hdr),
					   data, len);
			free(data);
		}

		return 1;
	}
	return 0;
}

void process_xevent_keypress(XKeyEvent * ev)
{
	struct msghdr hdr;
	struct msg_keypress k;
	CHECK_NONMANAGED_WINDOW(ev->window);
	last_input_window = conn;
	if (is_special_keypress(ev, conn->remote_winid))
		return;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.keycode = ev->keycode;
	hdr.type = MSG_KEYPRESS;
	hdr.window = conn->remote_winid;
	write_message(hdr, k);
//      fprintf(stderr, "win 0x%x(0x%x) type=%d keycode=%d\n",
//              (int) ev->window, hdr.window, k.type, k.keycode);
}

// debug routine
void dump_mapped()
{
	struct genlist *item = wid2conndata->next;
	for (; item != wid2conndata; item = item->next) {
		struct conndata *c = item->data;
		if (c->is_mapped) {
			fprintf(stderr,
				"id 0x%x(0x%x) w=0x%x h=0x%x rx=%d ry=%d ovr=%d\n",
				(int) c->local_winid,
				(int) c->remote_winid, c->width, c->height,
				c->x, c->y, c->override_redirect);
		}
	}
}

void process_xevent_button(XButtonEvent * ev)
{
	struct msghdr hdr;
	struct msg_button k;
	CHECK_NONMANAGED_WINDOW(ev->window);

// for debugging only, inactive
	if (0 && ev->button == 4) {
		dump_mapped();
		return;
	}

	last_input_window = conn;
	k.type = ev->type;

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.button = ev->button;
	hdr.type = MSG_BUTTON;
	hdr.window = conn->remote_winid;
	write_message(hdr, k);
	fprintf(stderr, "xside: win 0x%x(0x%x) type=%d button=%d\n",
		(int) ev->window, hdr.window, k.type, k.button);
}

void process_xevent_close(XID window)
{
	struct msghdr hdr;
	CHECK_NONMANAGED_WINDOW(window);
	hdr.type = MSG_CLOSE;
	hdr.window = conn->remote_winid;
	write_struct(hdr);
}

void send_configure(struct conndata *conn, int x, int y, int w, int h)
{
	struct msghdr hdr;
	struct msg_configure msg;
	hdr.type = MSG_CONFIGURE;
	hdr.window = conn->remote_winid;
	msg.height = h;
	msg.width = w;
	msg.x = x;
	msg.y = y;
	write_message(hdr, msg);
}

int fix_docked_xy(struct conndata *conn)
{

	/* docked window is reparented to root_win on vmside */
	XWindowAttributes attr;
	Window win;
	int x, y, ret = 0;
	XGetWindowAttributes(ghandles.display, conn->local_winid, &attr);
	if (XTranslateCoordinates
	    (ghandles.display, conn->local_winid, ghandles.root_win,
	     0, 0, &x, &y, &win) == True) {
		if (conn->x != x || conn->y != y)
			ret = 1;
		conn->x = x;
		conn->y = y;
	}
	return ret;
}

int force_on_screen(struct conndata *item, int border_width) {
	int do_move;

	if (item->x < border_width && item->x + item->width > 0) {
		item->x = border_width;
		do_move = 1;
	}
	if (item->y < border_width && item->y + item->height > 0) {
		item->y = border_width;
		do_move = 1;
	}
	if (item->x < ghandles.root_width && 
			item->x + item->width > ghandles.root_width - border_width) {
		item->width = ghandles.root_width - item->x - border_width;
		do_move = 1;
	}
	if (item->y < ghandles.root_height && 
			item->y + item->height > ghandles.root_height - border_width) {
		item->height = ghandles.root_height - item->y - border_width;
		do_move = 1;
	}
	return do_move;
}

void process_xevent_configure(XConfigureEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(ev->window);
//      fprintf(stderr, "process_xevent_configure, %d/%d, was"
//              "%d/%d\n", ev->width, ev->height,
//              conn->width, conn->height);
	if (conn->width == ev->width && conn->height == ev->height
	    && conn->x == ev->x && conn->y == ev->y)
		return;
	conn->width = ev->width;
	conn->height = ev->height;
	if (!conn->is_docked) {
		conn->x = ev->x;
		conn->y = ev->y;
	} else
		fix_docked_xy(conn);

// if AppVM has not unacknowledged previous resize msg, do not send another one
	if (conn->have_queued_configure)
		return;
	conn->have_queued_configure = 1;
	send_configure(conn, conn->x, conn->y, conn->width, conn->height);
}

void handle_configure_from_vm(Ghandles * g, struct conndata *item)
{
	struct msg_configure untrusted_conf;
	unsigned int x,y,width,height,override_redirect;
	int conf_changed;

	read_struct(untrusted_conf);
	fprintf(stderr, "handle_configure_from_vm, %d/%d, was"
		" %d/%d, ovr=%d\n", untrusted_conf.width, untrusted_conf.height,
		item->width, item->height, untrusted_conf.override_redirect);
	/* sanitize start */
	if (untrusted_conf.width > MAX_WINDOW_WIDTH)
		untrusted_conf.width = MAX_WINDOW_WIDTH;
	if (untrusted_conf.height > MAX_WINDOW_HEIGHT)
		untrusted_conf.height = MAX_WINDOW_HEIGHT;
	width = untrusted_conf.width;
	height = untrusted_conf.height;
	VERIFY(width >= 0 && height >= 0);
	if (untrusted_conf.override_redirect > 0)
		override_redirect = 1;
	else
		override_redirect = 0;
	VERIFY((int)untrusted_conf.x >= -g->root_width && (int)untrusted_conf.x <= 2*g->root_width);
	VERIFY((int)untrusted_conf.y >= -g->root_height && (int)untrusted_conf.y <= 2*g->root_height);
	x = untrusted_conf.x;
	y = untrusted_conf.y;
	/* sanitize end */
	if (item->width != width || item->height != height ||
	    item->x != x || item->y != y)
		conf_changed = 1;
	else
		conf_changed = 0;
	item->override_redirect = override_redirect;

	/* We do not allow a docked window to change its size, period. */
	if (item->is_docked) {
		if (conf_changed)
			send_configure(item, item->x, item->y, item->width,
				       item->height);
		item->have_queued_configure = 0;
		return;
	}


	if (item->have_queued_configure) {
		if (conf_changed) {
			send_configure(item, item->x, item->y, item->width,
				       item->height);
			return;
		} else {
			// same dimensions; this is an ack for our previously sent configure req
			item->have_queued_configure = 0;
		}
	}
	if (!conf_changed)
		return;
	item->width = width;
	item->height = height;
	item->x = x;
	item->y = y;
	if (item->override_redirect)
		// do not let menu window hide its color frame by moving outside of the screen
		// if it is located offscreen, then allow negative x/y
		force_on_screen(item, 0);
	XMoveResizeWindow(g->display, item->local_winid, item->x, item->y,
			  item->width, item->height);
}

void process_xevent_motion(XMotionEvent * ev)
{
	struct msghdr hdr;
	struct msg_motion k;
	CHECK_NONMANAGED_WINDOW(ev->window);

	if (conn->is_docked && fix_docked_xy(conn))
		send_configure(conn, conn->x, conn->y, conn->width,
			       conn->height);


	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.is_hint = ev->is_hint;
	hdr.type = MSG_MOTION;
	hdr.window = conn->remote_winid;
	write_message(hdr, k);
//      fprintf(stderr, "motion in 0x%x", ev->window);
}

void process_xevent_crossing(XCrossingEvent * ev)
{
	struct msghdr hdr;
	struct msg_crossing k;
	CHECK_NONMANAGED_WINDOW(ev->window);

	hdr.type = MSG_CROSSING;
	hdr.window = conn->remote_winid;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.mode = ev->mode;
	k.detail = ev->detail;
	k.focus = ev->focus;
	write_message(hdr, k);
}

void process_xevent_focus(XFocusChangeEvent * ev)
{
	struct msghdr hdr;
	struct msg_focus k;
	CHECK_NONMANAGED_WINDOW(ev->window);
	hdr.type = MSG_FOCUS;
	hdr.window = conn->remote_winid;
	k.type = ev->type;
	k.mode = ev->mode;
	k.detail = ev->detail;
	write_message(hdr, k);
	if (ev->type == FocusIn) {
		char keys[32];
		XQueryKeymap(ghandles.display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(hdr, keys);
	}
}

void do_shm_update(struct conndata *conn, int untrusted_x, int untrusted_y, int untrusted_w, int untrusted_h)
{
	int border_width = BORDER_WIDTH;
	int x,y,w,h;

	/* sanitize start */
	if (untrusted_x < 0 || untrusted_y < 0) {
		fprintf(stderr,
			"do_shm_update for 0x%x(remote 0x%x), x=%d, y=%d, w=%d, h=%d ?\n",
			(int) conn->local_winid, (int) conn->remote_winid,
			untrusted_x, untrusted_y, untrusted_w, untrusted_h);
		return;
	}
	x = min(untrusted_x, conn->image_width);
	y = min(untrusted_y, conn->image_height);
	w = min(max(untrusted_w, 0), conn->image_width - x);
	h = min(max(untrusted_h, 0), conn->image_height - y);
	/* sanitize end */

	if (!conn->override_redirect) {
		// Window Manager will take care of the frame...
		border_width = 0;
	}

	int do_border = 0;
	int hoff = 0, woff = 0, delta, i;
	if (!conn->image)
		return;
	/* when image larger than local window - place middle part of image in the
	 * window */
	if (conn->image_height > conn->height)
		hoff = (conn->image_height - conn->height) / 2;
	if (conn->image_width > conn->width)
		woff = (conn->image_width - conn->width) / 2;
	/* window contains only (forced) frame, so no content to update */
	if (conn->width < border_width * 2
	    || conn->height < border_width * 2)
		return;
	/* force frame to be visible: */
	/*   * left */
	delta = border_width - x;
	if (delta > 0) {
		w -= delta;
		x = border_width;
		do_border = 1;
	}
	/*   * right */
	delta = x + w - (conn->width - border_width);
	if (delta > 0) {
		w -= delta;
		do_border = 1;
	}
	/*   * top */
	delta = border_width - y;
	if (delta > 0) {
		h -= delta;
		y = border_width;
		do_border = 1;
	}
	/*   * bottom */
	delta = y + h - (conn->height - border_width);
	if (delta > 0) {
		h -= delta;
		do_border = 1;
	}

	/* again check if something left to update */
	if (w <= 0 || h <= 0)
		return;

	if (conn->is_docked) {
		char *data, *datap;
		size_t data_sz;
		int xp, yp;

		/* allocate image_width _bits_ for each image line */
		data_sz = (conn->image_width / 8 + 1) * conn->image_height;
		data = datap = calloc(1, data_sz);
		if (!data) {
			perror("malloc");
			exit(1);
		}

		/* Create local pixmap, put vmside image to it
		 * then get local image of the copy.
		 * This is needed because XGetPixel does not seem to work
		 * with XShmImage data. */
		Pixmap pixmap =
		    XCreatePixmap(ghandles.display, conn->local_winid,
				  conn->image_width, conn->image_height,
				  24);
		XShmPutImage(ghandles.display, pixmap, ghandles.context,
			     conn->image, 0, 0, 0, 0, conn->image_width,
			     conn->image_height, 0);
		XImage *image =
		    XGetImage(ghandles.display, pixmap, x, y, w, h,
			      0xFFFFFFFF, ZPixmap);
		/* Use top-left corner pixel color as transparency color */
		unsigned long back = XGetPixel(image, 0, 0);
		/* Generate data for transparency mask Bitmap */
		for (yp = y; yp < h; yp++) {
			assert(datap - data < data_sz);
			int step = 0;
			for (xp = x; xp < w; xp++) {
				if (XGetPixel(image, xp, yp) != back)
					*datap |= 1 << (step % 8);
				if (step % 8 == 7)
					datap++;
				step++;
			}
			/* ensure that new line will start at new byte */
			if ((step - 1) % 8 != 7)
				datap++;
		}
		Pixmap mask = XCreateBitmapFromData(ghandles.display,
						    conn->local_winid,
						    data, w, h);
		/* set trayicon background to white color */
		XFillRectangle(ghandles.display, conn->local_winid,
			       ghandles.tray_gc, 0, 0, conn->width,
			       conn->height);
		/* Paint clipped Image */
		XSetClipMask(ghandles.display, ghandles.context, mask);
		XPutImage(ghandles.display, conn->local_winid,
			  ghandles.context, image, x + woff, y + hoff, x,
			  y, w, h);
		/* Remove clipping */
		XSetClipMask(ghandles.display, ghandles.context, None);
		/* Draw VM color frame in case VM tries to cheat
		 * and puts its own background color */
		XDrawRectangle(ghandles.display, conn->local_winid,
			       ghandles.frame_gc, 0, 0,
			       conn->width - 1, conn->height - 1);

		XFreePixmap(ghandles.display, mask);
		XDestroyImage(image);
		XFreePixmap(ghandles.display, pixmap);
		free(data);
		return;
	} else
		XShmPutImage(ghandles.display, conn->local_winid,
			     ghandles.context, conn->image, x + woff,
			     y + hoff, x, y, w, h, 0);
	if (!do_border)
		return;
	for (i = 0; i < border_width; i++)
		XDrawRectangle(ghandles.display, conn->local_winid,
			       ghandles.frame_gc, i, i,
			       conn->width - 1 - 2 * i,
			       conn->height - 1 - 2 * i);

}

void process_xevent_expose(XExposeEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(ev->window);
	do_shm_update(conn, ev->x, ev->y, ev->width, ev->height);
}

void process_xevent_mapnotify(XMapEvent * ev)
{
	XWindowAttributes attr;
	CHECK_NONMANAGED_WINDOW(ev->window);
	if (conn->is_mapped)
		return;
	XGetWindowAttributes(ghandles.display, conn->local_winid, &attr);
	if (attr.map_state != IsViewable && !conn->is_docked) {
		/* Unmap windows that are not visible on vmside.
		 * WM may try to map non-viewable windows ie. when
		 * switching desktops.
		 */
		(void) XUnmapWindow(ghandles.display, conn->local_winid);
		fprintf(stderr, "WM tried to map 0x%x, revert\n",
			(int) conn->local_winid);
	} else {
		/* Tray windows shall be visible always */
		struct msghdr hdr;
		struct msg_map_info map_info;
		map_info.override_redirect = attr.override_redirect;
		hdr.type = MSG_MAP;
		hdr.window = conn->remote_winid;
		write_struct(hdr);
		write_struct(map_info);
	}
}

void process_xevent_xembed(XClientMessageEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(ev->window);
	fprintf(stderr, "_XEMBED message %ld\n", ev->data.l[1]);
	if (ev->data.l[1] == XEMBED_EMBEDDED_NOTIFY) {
		if (conn->is_docked < 2) {
			conn->is_docked = 2;
			if (!conn->is_mapped)
				XMapWindow(ghandles.display, ev->window);
		}
	}
}

void process_xevent()
{
	XEvent event_buffer;
	XNextEvent(ghandles.display, &event_buffer);
	switch (event_buffer.type) {
	case KeyPress:
	case KeyRelease:
		process_xevent_keypress((XKeyEvent *) & event_buffer);
		break;
	case ConfigureNotify:
		process_xevent_configure((XConfigureEvent *) &
					 event_buffer);
		break;
	case ButtonPress:
	case ButtonRelease:
		process_xevent_button((XButtonEvent *) & event_buffer);
		break;
	case MotionNotify:
		process_xevent_motion((XMotionEvent *) & event_buffer);
		break;
	case EnterNotify:
	case LeaveNotify:
		process_xevent_crossing((XCrossingEvent *) & event_buffer);
		break;
	case FocusIn:
	case FocusOut:
		process_xevent_focus((XFocusChangeEvent *) & event_buffer);
		break;
	case Expose:
		process_xevent_expose((XExposeEvent *) & event_buffer);
		break;
	case MapNotify:
		process_xevent_mapnotify((XMapEvent *) & event_buffer);
		break;
	case ClientMessage:
//              fprintf(stderr, "xclient, atom=%s\n",
//                      XGetAtomName(ghandles.display,
//                                   event_buffer.xclient.message_type));
		if (event_buffer.xclient.message_type ==
		    ghandles.xembed_message) {
			process_xevent_xembed((XClientMessageEvent *) &
					      event_buffer);
		} else if (event_buffer.xclient.data.l[0] ==
			   ghandles.wmDeleteMessage) {
			fprintf(stderr, "close for 0x%x\n",
				(int) event_buffer.xclient.window);
			process_xevent_close(event_buffer.xclient.window);
		}
		break;
	default:;
	}
}



void handle_shmimage(Ghandles * g, struct conndata *item)
{
	struct msg_shmimage untrusted_mx;

	read_struct(untrusted_mx);
	if (!item->is_mapped)
		return;
	/* WARNING: passing raw values, input validation is done inside of
	 * do_shm_update */
	do_shm_update(item, untrusted_mx.x, untrusted_mx.y, untrusted_mx.width,
			untrusted_mx.height);
}

int windows_count;
int windows_count_limit = 100;
void ask_whether_flooding()
{
	char text[1024];
	int ret;
	snprintf(text, sizeof(text),
		 "kdialog --yesnocancel "
		 "'VMapp \"%s\" has created %d windows; it looks numerous, "
		 "so it may be "
		 "a beginning of a DoS attack. Do you want to continue:'",
		 ghandles.vmname, windows_count);
	do {
		ret = system(text);
		ret = WEXITSTATUS(ret);
//              fprintf(stderr, "ret=%d\n", ret);
		switch (ret) {
		case 2:	/*cancel */
			break;
		case 1:	/* NO */
			exit(1);
		case 0:	/*YES */
			windows_count_limit += 100;
			break;
		default:
			fprintf(stderr, "Problems executing kdialog ?\n");
			exit(1);
		}
	} while (ret == 2);
}


void handle_create(Ghandles * g, XID window)
{
	struct conndata *item;
	struct genlist *l;
	struct msg_create untrusted_crt;
	XID parent;

	if (windows_count++ > windows_count_limit)
		ask_whether_flooding();
	item = (struct conndata *) calloc(1, sizeof(struct conndata));
	if (!item) {
		perror("malloc(item in handle_create)");
		exit(1);
	}
#if 0
	because of calloc item->image = 0;
	item->is_mapped = 0;
	item->local_winid = 0;
	item->dest = item->src = item->pix = 0;
#endif
	read_struct(untrusted_crt);
	/* sanitize start */
	VERIFY((int)untrusted_crt.width >= 0 && (int)untrusted_crt.height >= 0);
	item->width = min((int)untrusted_crt.width, MAX_WINDOW_WIDTH);
	item->height = min((int)untrusted_crt.height, MAX_WINDOW_HEIGHT);
	VERIFY((int)untrusted_crt.x >= -g->root_width && (int)untrusted_crt.x <= 2*g->root_width);
	VERIFY((int)untrusted_crt.y >= -g->root_height && (int)untrusted_crt.y <= 2*g->root_height);
	item->x = untrusted_crt.x;
	item->y = untrusted_crt.y;
	if (untrusted_crt.override_redirect)
		item->override_redirect = 1;
	else
		item->override_redirect = 0;
	parent = untrusted_crt.parent;
	/* sanitize end */
	item->remote_winid = window;
	list_insert(remote2local, window, item);
	l = list_lookup(remote2local, parent);
	if (l)
		item->parent = l->data;
	else
		item->parent = NULL;
	item->transient_for = NULL;
	item->local_winid = mkwindow(&ghandles, item);
	fprintf(stderr,
		"Created 0x%x(0x%x) parent 0x%x(0x%x) ovr=%d\n",
		(int) item->local_winid, (int) window,
		(int) (item->parent ? item->parent->local_winid : 0),
		(unsigned)parent, item->override_redirect);
	list_insert(wid2conndata, item->local_winid, item);
	/* do not allow to hide color frame off the screen */
    if (item->override_redirect && force_on_screen(item, 0))
		XMoveResizeWindow(ghandles.display, item->local_winid, item->x,
			    item->y, item->width, item->height);
}

void release_mapped_mfns(Ghandles * g, struct conndata *item);

void handle_destroy(Ghandles * g, struct genlist *l)
{
	struct genlist *l2;
	struct conndata *item = l->data;
	windows_count--;
	if (item == last_input_window)
		last_input_window = NULL;
	XDestroyWindow(g->display, item->local_winid);
	fprintf(stderr, " XDestroyWindow 0x%x\n", (int) item->local_winid);
	if (item->image)
		release_mapped_mfns(g, item);
	l2 = list_lookup(wid2conndata, item->local_winid);
	list_remove(l);
	list_remove(l2);
}

void sanitize_string_from_vm(unsigned char *untrusted_s)
{
	static unsigned char allowed_chars[] = { '-', '_', ' ', '.' };
	int i, ok;
	for (; *untrusted_s; untrusted_s++) {
		if (*untrusted_s >= '0' || *untrusted_s <= '9')
			continue;
		if (*untrusted_s >= 'a' || *untrusted_s <= 'z')
			continue;
		if (*untrusted_s >= 'A' || *untrusted_s <= 'Z')
			continue;
		ok = 0;
		for (i = 0; i < sizeof(allowed_chars); i++)
			if (*untrusted_s == allowed_chars[i])
				ok = 1;
		if (!ok)
			*untrusted_s = '_';
	}
}

void fix_menu(struct conndata *item)
{
	XSetWindowAttributes attr;

	attr.override_redirect = 1;
	XChangeWindowAttributes(ghandles.display, item->local_winid,
				CWOverrideRedirect, &attr);
	item->override_redirect = 1;

	// do not let menu window hide its color frame by moving outside of the screen
	// if it is located offscreen, then allow negative x/y
	if (force_on_screen(item, 0))
		XMoveResizeWindow(ghandles.display, item->local_winid, item->x,
			    item->y, item->width, item->height);
}

void handle_wmname(Ghandles * g, struct conndata *item)
{
	XTextProperty text_prop;
	struct msg_wmname untrusted_msg;
	char buf[sizeof(untrusted_msg.data) + 1];
	char *list[1] = { buf };

	read_struct(untrusted_msg);
	/* sanitize start */
	untrusted_msg.data[sizeof(untrusted_msg.data) - 1] = 0;
	sanitize_string_from_vm((unsigned char *) (untrusted_msg.data));
	snprintf(buf, sizeof(buf), "%s", untrusted_msg.data);
	/* sanitize end */
	fprintf(stderr, "set title for window 0x%x to %s\n",
		(int) item->local_winid, buf);
	XmbTextListToTextProperty(g->display, list, 1, XStringStyle,
				  &text_prop);
	XSetWMName(g->display, item->local_winid, &text_prop);
	XSetWMIconName(g->display, item->local_winid, &text_prop);
	XFree(text_prop.value);
}

void handle_map(struct conndata *item)
{
	struct genlist *trans;
	struct msg_map_info untrusted_txt;

	read_struct(untrusted_txt);
	item->is_mapped = 1;
	if (untrusted_txt.transient_for
	    && (trans = list_lookup(remote2local, untrusted_txt.transient_for))) {
		struct conndata *transdata = trans->data;
		item->transient_for = transdata;
		XSetTransientForHint(ghandles.display, item->local_winid,
				     transdata->local_winid);
	} else
		item->transient_for = 0;
	item->override_redirect = 0;
	if (untrusted_txt.override_redirect || item->is_docked)
		fix_menu(item);
	(void) XMapWindow(ghandles.display, item->local_winid);
}

void handle_dock(Ghandles * g, struct conndata *item)
{
	Window tray;
	fprintf(stderr, "docking window 0x%x\n", (int) item->local_winid);
	tray = XGetSelectionOwner(g->display, g->tray_selection);
	if (tray != None) {
		XClientMessageEvent msg;
		memset(&msg, 0, sizeof(msg));
		msg.type = ClientMessage;
		msg.window = tray;
		msg.message_type = g->tray_opcode;
		msg.format = 32;
		msg.data.l[0] = CurrentTime;
		msg.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
		msg.data.l[2] = item->local_winid;
		msg.display = g->display;
		XSendEvent(msg.display, msg.window, False, NoEventMask,
			   (XEvent *) & msg);
	}
	item->is_docked = 1;
}

void inter_appviewer_lock(int mode)
{
	int cmd;
	static int fd = 0;
	if (!fd) {
		fd = open("/var/run/qubes/appviewer.lock",
			  O_RDWR | O_CREAT, 0600);
		if (fd < 0) {
			perror("create lock");
			exit(1);
		}
	}
	if (mode)
		cmd = LOCK_EX;
	else
		cmd = LOCK_UN;
	if (flock(fd, cmd) < 0) {
		perror("lock");
		exit(1);
	}
}

void release_mapped_mfns(Ghandles * g, struct conndata *item)
{
	inter_appviewer_lock(1);
	g->shmcmd->shmid = item->shminfo.shmid;
	XShmDetach(g->display, &item->shminfo);
	XDestroyImage(item->image);
	XSync(g->display, False);
	inter_appviewer_lock(0);
	item->image = NULL;
}

char dummybuf[100];
void handle_mfndump(Ghandles * g, struct conndata *item)
{
	char untrusted_shmcmd_data_from_remote[4096 * SHM_CMD_NUM_PAGES];
	struct shm_cmd *untrusted_shmcmd =
	    (struct shm_cmd *) untrusted_shmcmd_data_from_remote;
	unsigned num_mfn, off;


	if (item->image)
		release_mapped_mfns(g, item);
	read_data(untrusted_shmcmd_data_from_remote, sizeof(struct shm_cmd));
	/* sanitize start */
	VERIFY(untrusted_shmcmd->num_mfn <= MAX_MFN_COUNT);
	num_mfn = untrusted_shmcmd->num_mfn;
	VERIFY((int)untrusted_shmcmd->width >= 0 && (int)untrusted_shmcmd->height >= 0);
	VERIFY((int)untrusted_shmcmd->width < MAX_WINDOW_WIDTH && (int)untrusted_shmcmd->height < MAX_WINDOW_HEIGHT);
	VERIFY(untrusted_shmcmd->off < 4096);
	off = untrusted_shmcmd->off;
	if (num_mfn * 4096 <
	    item->image->bytes_per_line * item->image->height + off) {
		fprintf(stderr,
			"handle_mfndump for window 0x%x(remote 0x%x)"
			" got too small num_mfn= 0x%x\n",
			(int) item->local_winid, (int) item->remote_winid,
			num_mfn);
		exit(1);
	}
	/* unused for now: VERIFY(untrusted_shmcmd->bpp == 24); */
	/* sanitize end */
	read_data((char *) untrusted_shmcmd->mfns,
		  SIZEOF_SHARED_MFN * num_mfn);
	item->image =
	    XShmCreateImage(g->display,
			    DefaultVisual(g->display, g->screen), 24,
			    ZPixmap, NULL, &item->shminfo,
			    item->image_width, item->image_height);
	if (!item->image) {
		perror("XShmCreateImage");
		exit(1);
	}
	// temporary shmid; see shmoverride/README
	item->shminfo.shmid = shmget(IPC_PRIVATE, 1, IPC_CREAT | 0700);
	if (item->shminfo.shmid < 0) {
		perror("shmget");
		exit(1);
	}
	/* ensure that _every_ not sanitized field is overrided by some trusted
	 * value */
	untrusted_shmcmd->shmid = item->shminfo.shmid;
	untrusted_shmcmd->domid = g->domid;
	inter_appviewer_lock(1);
	memcpy(g->shmcmd, untrusted_shmcmd_data_from_remote,
	       4096 * SHM_CMD_NUM_PAGES);
	item->shminfo.shmaddr = item->image->data = dummybuf;
	item->shminfo.readOnly = True;
	XSync(g->display, False);
	if (!XShmAttach(g->display, &item->shminfo)) {
		fprintf(stderr,
			"XShmAttach failed for window 0x%x(remote 0x%x)\n",
			(int) item->local_winid, (int) item->remote_winid);
	}
	XSync(g->display, False);
	g->shmcmd->shmid = g->cmd_shmid;
	inter_appviewer_lock(0);
	shmctl(item->shminfo.shmid, IPC_RMID, 0);
}

void handle_message()
{
	struct msghdr untrusted_hdr;
	uint32_t type;
	XID window;
	struct genlist *l;
	struct conndata *item = 0;

	read_struct(untrusted_hdr);
	VERIFY(untrusted_hdr.type > MSG_MIN && untrusted_hdr.type < MSG_MAX);
	/* sanitized msg type */
	type = untrusted_hdr.type;
	if (type == MSG_CLIPBOARD_DATA) {
		/* window field has special meaning here */
		handle_clipboard_data(untrusted_hdr.window);
		return;
	}
	l = list_lookup(remote2local, untrusted_hdr.window);
	if (type == MSG_CREATE) {
		if (l) {
			fprintf(stderr,
				"CREATE for already existing window id 0x%x?\n",
				untrusted_hdr.window);
			exit(1);
		}
		window = untrusted_hdr.window;
	} else {
		if (!l) {
			fprintf(stderr,
				"msg 0x%x without CREATE for 0x%x\n",
				type, untrusted_hdr.window);
			exit(1);
		}
		item = l->data;
		/* not needed as it is in item struct
		window = untrusted_hdr.window;
		*/
	}

	switch (type) {
	case MSG_CREATE:
		handle_create(&ghandles, window);
		break;
	case MSG_DESTROY:
		handle_destroy(&ghandles, l);
		break;
	case MSG_MAP:
		handle_map(item);
		break;
	case MSG_UNMAP:
		item->is_mapped = 0;
		(void) XUnmapWindow(ghandles.display, item->local_winid);
		break;
	case MSG_CONFIGURE:
		handle_configure_from_vm(&ghandles, item);
		break;
	case MSG_MFNDUMP:
		handle_mfndump(&ghandles, item);
		break;
	case MSG_SHMIMAGE:
		handle_shmimage(&ghandles, item);
		break;
	case MSG_WMNAME:
		handle_wmname(&ghandles, item);
		break;
	case MSG_DOCK:
		handle_dock(&ghandles, item);
		break;
	default:
		fprintf(stderr, "got msg type %d\n", type);
		exit(1);
	}
}

void send_cmd_to_vm(char *cmd)
{
	struct msghdr hdr;
	struct msg_execute exec_data;

	hdr.type = MSG_EXECUTE;
	hdr.window = 0;
	strncpy(exec_data.cmd, cmd, sizeof(exec_data.cmd));
	exec_data.cmd[sizeof(exec_data.cmd) - 1] = 0;
	write_message(hdr, exec_data);
}

static int volatile signal_caught;
void dummy_signal_handler(int x)
{
	signal_caught = 1;
}

void print_backtrace(void)
{
	void *array[100];
	size_t size;
	char **strings;
	size_t i;

	size = backtrace(array, 100);
	strings = backtrace_symbols(array, size);

	fprintf(stderr, "Obtained %zd stack frames.\n", size);

	for (i = 0; i < size; i++)
		printf("%s\n", strings[i]);

	free(strings);
}

void release_all_mapped_mfns()
{
	struct genlist *curr;
	fprintf(stderr, "release_all_mapped_mfns running\n");
	print_backtrace();
	for (curr = wid2conndata->next; curr != wid2conndata;
	     curr = curr->next) {
		struct conndata *item = curr->data;
		if (item->image)
			release_mapped_mfns(&ghandles, item);
	}
}

void exec_pacat(int domid)
{
	int i, fd;
	char domid_txt[20];
	char logname[80];
	snprintf(domid_txt, sizeof domid_txt, "%d", domid);
	snprintf(logname, sizeof logname, "/var/log/qubes/pacat.%d.log",
		 domid);
	switch (fork()) {
	case -1:
		perror("fork pacat");
		exit(1);
	case 0:
		for (i = 0; i < 256; i++)
			close(i);
		fd = open("/dev/null", O_RDWR);
		for (i = 0; i <= 1; i++)
			dup2(fd, i);
		umask(0007);
		fd = open(logname, O_WRONLY | O_CREAT | O_TRUNC, 0640);
		umask(0077);
		execl("/usr/bin/pacat-simple-vchan", "pacat-simple-vchan",
		      domid_txt, NULL);
		perror("execl");
		exit(1);
	default:;
	}
}

void send_xconf(Ghandles * g)
{
	struct msg_xconf xconf;
	XWindowAttributes attr;
	XGetWindowAttributes(g->display, g->root_win, &attr);
	xconf.w = _VIRTUALX(attr.width);
	xconf.h = attr.height;
	xconf.depth = attr.depth;
	xconf.mem = xconf.w * xconf.h * 4 / 1024 + 1;
	write_struct(xconf);
}

void get_protocol_version()
{
	uint32_t untrusted_version;
	char message[1024];
	read_struct(untrusted_version);
	if (untrusted_version == QUBES_GUID_PROTOCOL_VERSION)
		return;
	snprintf(message, sizeof message, "kdialog --sorry \"The remote "
		 "protocol version is %d, the local protocol version is %d. Upgrade "
		 "qubes-gui-dom0 (in dom0) and qubes-gui-vm (in template VM) packages "
		 "so that they provide compatible/latest software. You can run 'xm console "
		 "vmname' (as root) to access shell prompt in the VM.\"",
		 untrusted_version, QUBES_GUID_PROTOCOL_VERSION);
	system(message);
	exit(1);
}

void get_frame_gc(Ghandles * g, char *name)
{
	XGCValues values;
	XColor fcolor, dummy;
	if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
		unsigned int rgb = strtoul(name, 0, 16);
		fcolor.blue = (rgb & 0xff) * 257;
		rgb >>= 8;
		fcolor.green = (rgb & 0xff) * 257;
		rgb >>= 8;
		fcolor.red = (rgb & 0xff) * 257;
		XAllocColor(g->display,
			    XDefaultColormap(g->display, g->screen),
			    &fcolor);
	} else
		XAllocNamedColor(g->display,
				 XDefaultColormap(g->display, g->screen),
				 name, &fcolor, &dummy);
	values.foreground = fcolor.pixel;
	g->frame_gc =
	    XCreateGC(g->display, g->root_win, GCForeground, &values);
}

void get_tray_gc(Ghandles * g)
{
	XGCValues values;
	values.foreground = WhitePixel(g->display, g->screen);
	g->tray_gc =
	    XCreateGC(g->display, g->root_win, GCForeground, &values);
}

void wait_for_connection_in_parent(int *pipe_notify)
{
	// inside the parent process
	// wait for daemon to get connection with AppVM
	struct pollfd pipe_pollfd;
	int tries, ret;

	fprintf(stderr, "Connecting to VM's GUI agent: ");
	close(pipe_notify[1]);	// close the writing end
	pipe_pollfd.fd = pipe_notify[0];
	pipe_pollfd.events = POLLIN;

	for (tries = 0;; tries++) {
		fprintf(stderr, ".");
		ret = poll(&pipe_pollfd, 1, 1000);
		if (ret < 0) {
			perror("poll");
			exit(1);
		}
		if (ret > 0) {
			if (pipe_pollfd.revents == POLLIN)
				break;
			fprintf(stderr, "exiting\n");
			exit(1);
		}
		if (tries >= 45) {
			fprintf(stderr,
				"\nHmm... this takes more time than usual --"
				" is the VM running?\n");
			fprintf(stderr, "Connecting to VM's GUI agent: ");
			tries = 0;
		}

	}
	fprintf(stderr, "connected\n");
	exit(0);
}

#if 0
void setup_icon(char *xpmfile)
{
	Pixmap dummy;
	XpmAttributes attributes;
	int ret;
	Display *dis = ghandles.display;
	attributes.valuemask = XpmColormap;
	attributes.x_hotspot = 0;
	attributes.y_hotspot = 0;
	attributes.depth = DefaultDepth(dis, DefaultScreen(dis));
	attributes.colormap = DefaultColormap(dis, DefaultScreen(dis));
	attributes.valuemask = XpmSize;

	ret = XpmReadFileToPixmap
	    (dis, ghandles.root_win, xpmfile, &ghandles.icon,
	     &dummy, &attributes);
	if (ret != XpmSuccess) {
		fprintf(stderr, "XpmReadFileToPixmap returned %d\n", ret);
		ghandles.icon = None;
	}
}
#endif
void usage()
{
	fprintf(stderr,
		"usage: qubes_quid -d domain_id [-e command] [-c color] [-l label_index] [-i icon name, no suffix]\n");
}

void parse_cmdline(int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "d:e:c:l:i:")) != -1) {
		switch (opt) {
		case 'd':
			ghandles.domid = atoi(optarg);
			break;
		case 'e':
			ghandles.execute_cmd_in_vm = 1;
			strncpy(ghandles.cmd_for_vm,
				optarg, sizeof(ghandles.cmd_for_vm));
			break;
		case 'c':
			ghandles.cmdline_color = optarg;
			break;
		case 'l':
			ghandles.label_index = strtoul(optarg, 0, 0);
			break;
		case 'i':
			ghandles.cmdline_icon = optarg;
			break;
		default:
			usage();
			exit(1);
		}
	}
	if (!ghandles.domid) {
		fprintf(stderr, "domid=0?");
		exit(1);
	}
}

void set_alive_flag(int domid)
{
	char buf[256];
	int fd;
	snprintf(buf, sizeof(buf), "/var/run/qubes/guid_running.%d",
		 domid);
	fd = open(buf, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
	close(fd);
}

int main(int argc, char **argv)
{
	int xfd;
	char *vmname;
	FILE *f;
	int childpid;
	int pipe_notify[2];
	char dbg_log[256];
	int logfd;
	parse_cmdline(argc, argv);


	// daemonize...
	if (pipe(pipe_notify) < 0) {
		perror("canot create pipe:");
		exit(1);
	}

	childpid = fork();
	if (childpid < 0) {
		fprintf(stderr, "Cannot fork :(\n");
		exit(1);
	} else if (childpid > 0)
		wait_for_connection_in_parent(pipe_notify);

	// inside the daemonized process...
	f = fopen("/var/run/shm.id", "r");
	if (!f) {
		fprintf(stderr,
			"Missing /var/run/shm.id; run X with preloaded shmoverride\n");
		exit(1);
	}
	fscanf(f, "%d", &ghandles.cmd_shmid);
	fclose(f);
	ghandles.shmcmd = shmat(ghandles.cmd_shmid, 0, 0);
	if (ghandles.shmcmd == (void *) (-1UL)) {
		fprintf(stderr,
			"Invalid or stale shm id 0x%x in /var/run/shm.id\n",
			ghandles.cmd_shmid);
		exit(1);
	}

	close(0);
	snprintf(dbg_log, sizeof(dbg_log),
		 "/var/log/qubes/qubes.%d.log", ghandles.domid);
	umask(0007);
	logfd = open(dbg_log, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	umask(0077);
	dup2(logfd, 1);
	dup2(logfd, 2);

	chdir("/var/run/qubes");
	errno = 0;
	if (setsid() < 0) {
		perror("setsid()");
		exit(1);
	}
	mkghandles(&ghandles);
	get_frame_gc(&ghandles, ghandles.cmdline_color ? : "red");
	get_tray_gc(&ghandles);
#if 0
	if (ghandles.cmdline_icon)
		setup_icon(ghandles.cmdline_icon);
	else
		ghandles.icon = None;
#endif
	remote2local = list_new();
	wid2conndata = list_new();
	XSetErrorHandler(dummy_handler);
	vmname = peer_client_init(ghandles.domid, 6000);
	exec_pacat(ghandles.domid);
	setuid(getuid());
	set_alive_flag(ghandles.domid);
	write(pipe_notify[1], "Q", 1);	// let the parent know we connected sucessfully

	signal(SIGTERM, dummy_signal_handler);
	atexit(release_all_mapped_mfns);

	strncpy(ghandles.vmname, vmname, sizeof(ghandles.vmname));
	xfd = ConnectionNumber(ghandles.display);

	get_protocol_version();
	send_xconf(&ghandles);

	if (ghandles.execute_cmd_in_vm) {
		fprintf(stderr,
			"Sending cmd to execute: %s\n",
			ghandles.cmd_for_vm);
		send_cmd_to_vm(ghandles.cmd_for_vm);
	}

	for (;;) {
		int select_fds[2] = { xfd };
		fd_set retset;
		int busy;
		if (signal_caught) {
			fprintf(stderr, "exiting on signal...\n");
			exit(0);
		}
		do {
			busy = 0;
			if (XPending(ghandles.display)) {
				process_xevent();
				busy = 1;
			}
			if (read_ready()) {
				handle_message();
				busy = 1;
			}
		} while (busy);
		wait_for_vchan_or_argfd(1, select_fds, &retset);
	}
	return 0;
}
