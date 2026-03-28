/*
 * Linux (Wayland/COSMIC): full-desktop screenshot via portal, then a custom
 * region selector rendered with GTK4 layer-shell surfaces pinned to each output.
 */
#include <cairo.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

typedef enum {
  HT_NONE = -1,
  HT_TL = 0,
  HT_T,
  HT_TR,
  HT_L,
  HT_R,
  HT_BL,
  HT_B,
  HT_BR
} HandleId;

typedef struct {
  int x, y, w, h;
} IRect;

typedef struct {
  int x1, y1, x2, y2;
} SelRect;

typedef struct {
  GdkPixbuf *capture;
  cairo_surface_t *capture_surface;
  int cap_w, cap_h;
  GdkRectangle virt;

  gboolean have_sel, selecting, resizing, moving;
  SelRect sel, resize_anchor;
  int drag_sx, drag_sy, drag_cx, drag_cy;
  int move_off_x, move_off_y;
  HandleId active_handle;
} OverlayState;

typedef struct {
  GtkWidget *win;
  GtkWidget *da;
  GdkRectangle geom;
  int top_inset;
} OverlayWindow;

typedef struct {
  guint sub_id;
  char *request_path;
  GDBusConnection *bus;
} PortalWait;

static OverlayState g_ov;
static OverlayWindow *g_owins;
static int g_owin_count;
static GtkApplication *g_app;
static gboolean g_session_ending;

static void close_overlay_and_quit(void);

static const double OVERLAY_ALPHA = 0.45;
static const int HANDLE_SIZE = 3;
static const int BORDER_WIDTH = 2;
static const int MIN_SEL_SIZE = 2;

static IRect sel_to_irect(SelRect s) {
  IRect r;
  r.x = MIN(s.x1, s.x2);
  r.y = MIN(s.y1, s.y2);
  r.w = MAX(s.x1, s.x2) - r.x;
  r.h = MAX(s.y1, s.y2) - r.y;
  return r;
}

static gboolean point_in_geom(const GdkRectangle *geom, int x, int y) {
  return x >= geom->x && x < geom->x + geom->width && y >= geom->y &&
         y < geom->y + geom->height;
}

static GdkRectangle selection_label_monitor(IRect sel) {
  int gx = g_ov.virt.x + sel.x + sel.w / 2;
  int gy = g_ov.virt.y + sel.y + sel.h / 2;
  for (int i = 0; i < g_owin_count; i++) {
    if (point_in_geom(&g_owins[i].geom, gx, gy))
      return g_owins[i].geom;
  }

  if (g_owin_count > 0)
    return g_owins[0].geom;
  return g_ov.virt;
}

static void get_handle_centers(const SelRect *s, int px[8], int py[8]) {
  int l = MIN(s->x1, s->x2), r = MAX(s->x1, s->x2);
  int t = MIN(s->y1, s->y2), b = MAX(s->y1, s->y2);
  int cx = (l + r) / 2, cy = (t + b) / 2;
  px[HT_TL] = l;
  py[HT_TL] = t;
  px[HT_T] = cx;
  py[HT_T] = t;
  px[HT_TR] = r;
  py[HT_TR] = t;
  px[HT_L] = l;
  py[HT_L] = cy;
  px[HT_R] = r;
  py[HT_R] = cy;
  px[HT_BL] = l;
  py[HT_BL] = b;
  px[HT_B] = cx;
  py[HT_B] = b;
  px[HT_BR] = r;
  py[HT_BR] = b;
}

static gboolean hit_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static HandleId hit_test(const SelRect *s, int x, int y) {
  IRect n = sel_to_irect(*s);
  if (n.w < 1 || n.h < 1)
    return HT_NONE;
  int px[8], py[8];
  get_handle_centers(s, px, py);
  const int hs = HANDLE_SIZE;
  for (int i = 0; i < 8; i++) {
    if (hit_in_rect(x, y, px[i] - hs, py[i] - hs, hs * 2, hs * 2))
      return (HandleId)i;
  }
  const int edge = HANDLE_SIZE + 2;
  int l = n.x, r = n.x + n.w, t = n.y, b = n.y + n.h;
  if (hit_in_rect(x, y, l + edge, t - edge, (r - l) - 2 * edge, 2 * edge))
    return HT_T;
  if (hit_in_rect(x, y, l + edge, b - edge, (r - l) - 2 * edge, 2 * edge))
    return HT_B;
  if (hit_in_rect(x, y, l - edge, t + edge, 2 * edge, (b - t) - 2 * edge))
    return HT_L;
  if (hit_in_rect(x, y, r - edge, t + edge, 2 * edge, (b - t) - 2 * edge))
    return HT_R;
  return HT_NONE;
}

static gboolean pt_in_sel(const SelRect *s, int x, int y) {
  IRect n = sel_to_irect(*s);
  return hit_in_rect(x, y, n.x, n.y, n.w, n.h);
}

static HandleId swap_h(HandleId h) {
  switch (h) {
  case HT_L:
    return HT_R;
  case HT_R:
    return HT_L;
  case HT_TL:
    return HT_TR;
  case HT_TR:
    return HT_TL;
  case HT_BL:
    return HT_BR;
  case HT_BR:
    return HT_BL;
  default:
    return h;
  }
}

static HandleId swap_v(HandleId h) {
  switch (h) {
  case HT_T:
    return HT_B;
  case HT_B:
    return HT_T;
  case HT_TL:
    return HT_BL;
  case HT_BL:
    return HT_TL;
  case HT_TR:
    return HT_BR;
  case HT_BR:
    return HT_TR;
  default:
    return h;
  }
}

static void resize_robust(HandleId *h_io, int px, int py, SelRect *anchor,
                          SelRect *out_sel) {
  HandleId h = *h_io;
  int L = MIN(anchor->x1, anchor->x2), R = MAX(anchor->x1, anchor->x2);
  int T = MIN(anchor->y1, anchor->y2), B = MAX(anchor->y1, anchor->y2);

  if (h == HT_R || h == HT_TR || h == HT_BR) {
    if (px < L) {
      R = L;
      h = swap_h(h);
      anchor->x1 = anchor->x2 = R;
    } else {
      R = (px < L + MIN_SEL_SIZE) ? (L + MIN_SEL_SIZE) : px;
    }
  } else if (h == HT_L || h == HT_TL || h == HT_BL) {
    if (px > R) {
      L = R;
      h = swap_h(h);
      anchor->x1 = anchor->x2 = L;
    } else {
      L = (px > R - MIN_SEL_SIZE) ? (R - MIN_SEL_SIZE) : px;
    }
  }

  if (h == HT_B || h == HT_BL || h == HT_BR) {
    if (py < T) {
      B = T;
      h = swap_v(h);
      anchor->y1 = anchor->y2 = B;
    } else {
      B = (py < T + MIN_SEL_SIZE) ? (T + MIN_SEL_SIZE) : py;
    }
  } else if (h == HT_T || h == HT_TL || h == HT_TR) {
    if (py > B) {
      T = B;
      h = swap_v(h);
      anchor->y1 = anchor->y2 = T;
    } else {
      T = (py > B - MIN_SEL_SIZE) ? (B - MIN_SEL_SIZE) : py;
    }
  }

  out_sel->x1 = L;
  out_sel->x2 = R;
  out_sel->y1 = T;
  out_sel->y2 = B;
  *h_io = h;
}

static const char *cursor_name_for_handle(HandleId h) {
  switch (h) {
  case HT_T:
  case HT_B:
    return "ns-resize";
  case HT_L:
  case HT_R:
    return "ew-resize";
  case HT_TL:
  case HT_BR:
    return "nwse-resize";
  case HT_TR:
  case HT_BL:
    return "nesw-resize";
  default:
    return "crosshair";
  }
}

static OverlayWindow *overlay_window_from_widget(GtkWidget *w) {
  return g_object_get_data(G_OBJECT(w), "overlay-window");
}

static void overlay_global_xy(GtkWidget *w, double lx, double ly, int *gx,
                              int *gy) {
  OverlayWindow *ow = overlay_window_from_widget(w);
  int off_x = 0;
  int off_y = 0;
  if (ow) {
    off_x = ow->geom.x - g_ov.virt.x;
    int alloc_h = gtk_widget_get_height(w);
    ow->top_inset = MAX(0, ow->geom.height - alloc_h);
    off_y = ow->geom.y - g_ov.virt.y + ow->top_inset;
  }
  *gx = (int)lx + off_x;
  *gy = (int)ly + off_y;
}

static void queue_draw_all(void) {
  for (int i = 0; i < g_owin_count; i++) {
    if (g_owins[i].da)
      gtk_widget_queue_draw(g_owins[i].da);
  }
}

static void set_cursor_all(const char *name) {
  for (int i = 0; i < g_owin_count; i++) {
    if (g_owins[i].da)
      gtk_widget_set_cursor_from_name(g_owins[i].da, name);
  }
}

static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pixbuf) {
  int width = gdk_pixbuf_get_width(pixbuf);
  int height = gdk_pixbuf_get_height(pixbuf);
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    return surface;

  unsigned char *dst = cairo_image_surface_get_data(surface);
  int dst_stride = cairo_image_surface_get_stride(surface);
  guchar *src = gdk_pixbuf_get_pixels(pixbuf);
  int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
  int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
  gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);

  for (int y = 0; y < height; y++) {
    guint32 *dst_row = (guint32 *)(dst + y * dst_stride);
    guchar *src_row = src + y * src_stride;
    for (int x = 0; x < width; x++) {
      guchar r = src_row[x * n_channels + 0];
      guchar g = src_row[x * n_channels + 1];
      guchar b = src_row[x * n_channels + 2];
      guchar a = has_alpha ? src_row[x * n_channels + 3] : 255;

      guint32 pr = (guint32)((r * a + 127) / 255);
      guint32 pg = (guint32)((g * a + 127) / 255);
      guint32 pb = (guint32)((b * a + 127) / 255);
      dst_row[x] = (a << 24) | (pr << 16) | (pg << 8) | pb;
    }
  }

  cairo_surface_mark_dirty(surface);
  return surface;
}

static void overlay_cleanup(void) {
  if (g_ov.capture) {
    g_object_unref(g_ov.capture);
    g_ov.capture = NULL;
  }
  if (g_ov.capture_surface) {
    cairo_surface_destroy(g_ov.capture_surface);
    g_ov.capture_surface = NULL;
  }
  free(g_owins);
  g_owins = NULL;
  g_owin_count = 0;
}

static void quit_capture_session(void) {
  if (g_session_ending)
    return;
  g_session_ending = TRUE;
  if (g_app) {
    g_application_release(G_APPLICATION(g_app));
    g_application_quit(G_APPLICATION(g_app));
  }
}

static void destroy_overlay_windows(void) {
  OverlayWindow *wins = g_owins;
  int count = g_owin_count;
  g_owins = NULL;
  g_owin_count = 0;
  for (int i = 0; i < count; i++) {
    if (wins[i].win)
      gtk_window_destroy(GTK_WINDOW(wins[i].win));
  }
  free(wins);
}

static void close_overlay_and_quit(void) {
  if (g_session_ending)
    return;
  g_session_ending = TRUE;
  destroy_overlay_windows();
  overlay_cleanup();
  if (g_app) {
    g_application_release(G_APPLICATION(g_app));
    g_application_quit(G_APPLICATION(g_app));
  }
}

static gboolean copy_selection_to_clipboard(void) {
  if (!g_ov.have_sel)
    return FALSE;
  IRect n = sel_to_irect(g_ov.sel);
  if (n.w <= 0 || n.h <= 0)
    return FALSE;

  GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(g_ov.capture, n.x, n.y, n.w, n.h);
  if (!sub)
    return FALSE;

  gchar *buf = NULL;
  gsize len = 0;
  GError *err = NULL;
  if (!gdk_pixbuf_save_to_buffer(sub, &buf, &len, "png", &err, NULL)) {
    fprintf(stderr, "screenshot: encode png failed: %s\n",
            err ? err->message : "?");
    g_clear_error(&err);
    g_object_unref(sub);
    return FALSE;
  }

  int fds[2];
  if (pipe(fds) != 0) {
    fprintf(stderr, "screenshot: failed to create pipe for wl-copy\n");
    g_free(buf);
    g_object_unref(sub);
    return FALSE;
  }

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "screenshot: failed to fork wl-copy\n");
    close(fds[0]);
    close(fds[1]);
    g_free(buf);
    g_object_unref(sub);
    return FALSE;
  }

  if (pid == 0) {
    dup2(fds[0], STDIN_FILENO);
    close(fds[0]);
    close(fds[1]);
    execlp("wl-copy", "wl-copy", "--type", "image/png", (char *)NULL);
    _exit(127);
  }

  close(fds[0]);
  gsize total = 0;
  while (total < len) {
    ssize_t n = write(fds[1], buf + total, len - total);
    if (n <= 0)
      break;
    total += (gsize)n;
  }
  close(fds[1]);

  int st = 0;
  waitpid(pid, &st, 0);
  g_free(buf);
  g_object_unref(sub);
  if (total != len || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    fprintf(stderr, "screenshot: wl-copy failed\n");
    return FALSE;
  }
  return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)user_data;
  if (keyval == GDK_KEY_Escape) {
    close_overlay_and_quit();
    return TRUE;
  }
  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    copy_selection_to_clipboard();
    close_overlay_and_quit();
    return TRUE;
  }
  if ((state & GDK_CONTROL_MASK) &&
      (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
    copy_selection_to_clipboard();
    close_overlay_and_quit();
    return TRUE;
  }
  return FALSE;
}

static void on_click_pressed(GtkGestureClick *gesture, int n_press, double x,
                             double y, gpointer user_data) {
  (void)n_press;
  GtkWidget *w = GTK_WIDGET(user_data);
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  if (button == 3) {
    close_overlay_and_quit();
    return;
  }
  if (button != 1)
    return;

  int gx = 0, gy = 0;
  overlay_global_xy(w, x, y, &gx, &gy);
  SelRect *sr = &g_ov.sel;

  if (g_ov.have_sel) {
    HandleId h = hit_test(sr, gx, gy);
    if (h != HT_NONE) {
      g_ov.resizing = TRUE;
      g_ov.selecting = g_ov.moving = FALSE;
      g_ov.active_handle = h;
      g_ov.resize_anchor = *sr;
      return;
    }
    if (pt_in_sel(sr, gx, gy)) {
      g_ov.moving = TRUE;
      g_ov.selecting = g_ov.resizing = FALSE;
      IRect n = sel_to_irect(*sr);
      g_ov.move_off_x = gx - n.x;
      g_ov.move_off_y = gy - n.y;
      return;
    }
  }

  g_ov.selecting = TRUE;
  g_ov.resizing = g_ov.moving = FALSE;
  g_ov.have_sel = TRUE;
  g_ov.drag_sx = g_ov.drag_cx = gx;
  g_ov.drag_sy = g_ov.drag_cy = gy;
  sr->x1 = sr->x2 = gx;
  sr->y1 = sr->y2 = gy;
  queue_draw_all();
}

static void on_click_released(GtkGestureClick *gesture, int n_press, double x,
                              double y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  (void)user_data;
  g_ov.selecting = g_ov.resizing = g_ov.moving = FALSE;
  queue_draw_all();
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y,
                      gpointer user_data) {
  (void)controller;
  GtkWidget *w = GTK_WIDGET(user_data);
  int gx = 0, gy = 0;
  overlay_global_xy(w, x, y, &gx, &gy);

  SelRect *sr = &g_ov.sel;
  int cw = g_ov.cap_w;
  int ch = g_ov.cap_h;

  if (g_ov.selecting) {
    g_ov.drag_cx = gx;
    g_ov.drag_cy = gy;
    sr->x1 = g_ov.drag_sx;
    sr->y1 = g_ov.drag_sy;
    sr->x2 = gx;
    sr->y2 = gy;
    queue_draw_all();
    return;
  }

  if (g_ov.resizing) {
    SelRect tmp = *sr;
    HandleId h = g_ov.active_handle;
    resize_robust(&h, gx, gy, &g_ov.resize_anchor, &tmp);
    g_ov.active_handle = h;
    *sr = tmp;
    if (sr->x1 < 0)
      sr->x1 = 0;
    if (sr->y1 < 0)
      sr->y1 = 0;
    if (sr->x2 > cw)
      sr->x2 = cw;
    if (sr->y2 > ch)
      sr->y2 = ch;
    queue_draw_all();
    return;
  }

  if (g_ov.moving) {
    IRect n = sel_to_irect(*sr);
    int w0 = n.w, h0 = n.h;
    int nl = gx - g_ov.move_off_x, nt = gy - g_ov.move_off_y;
    if (nl < 0)
      nl = 0;
    if (nt < 0)
      nt = 0;
    if (nl + w0 > cw)
      nl = cw - w0;
    if (nt + h0 > ch)
      nt = ch - h0;
    sr->x1 = nl;
    sr->y1 = nt;
    sr->x2 = nl + w0;
    sr->y2 = nt + h0;
    queue_draw_all();
    return;
  }

  if (g_ov.have_sel) {
    HandleId h = hit_test(sr, gx, gy);
    const char *name =
        (h != HT_NONE) ? cursor_name_for_handle(h)
                       : pt_in_sel(sr, gx, gy) ? "all-scroll" : "crosshair";
    set_cursor_all(name);
  }
}

static void draw_overlay(GtkDrawingArea *area, cairo_t *cr, int width,
                         int height, gpointer user_data) {
  (void)area;
  OverlayWindow *ow = user_data;
  int off_x = ow->geom.x - g_ov.virt.x;
  ow->top_inset = MAX(0, ow->geom.height - height);
  int off_y = ow->geom.y - g_ov.virt.y + ow->top_inset;

  cairo_set_source_surface(cr, g_ov.capture_surface, -off_x, -off_y);
  cairo_paint(cr);

  cairo_set_source_rgba(cr, 0, 0, 0, OVERLAY_ALPHA);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (!g_ov.have_sel)
    return;

  SelRect sr = g_ov.sel;
  IRect n = sel_to_irect(sr);
  int lx = n.x - off_x;
  int ly = n.y - off_y;

  cairo_set_source_surface(cr, g_ov.capture_surface, -off_x, -off_y);
  cairo_rectangle(cr, lx, ly, n.w, n.h);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1, 1, 1, 1);
  cairo_set_line_width(cr, BORDER_WIDTH);
  cairo_rectangle(cr, lx + 0.5, ly + 0.5, n.w - 1, n.h - 1);
  cairo_stroke(cr);

  int px[8], py[8];
  get_handle_centers(&sr, px, py);
  cairo_set_source_rgb(cr, 0, 0, 0);
  for (int i = 0; i < 8; i++) {
    cairo_rectangle(cr, px[i] - off_x - HANDLE_SIZE,
                    py[i] - off_y - HANDLE_SIZE, HANDLE_SIZE * 2,
                    HANDLE_SIZE * 2);
    cairo_fill(cr);
  }
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_set_line_width(cr, 1);
  for (int i = 0; i < 8; i++) {
    cairo_rectangle(cr, px[i] - off_x - HANDLE_SIZE + 0.5,
                    py[i] - off_y - HANDLE_SIZE + 0.5, HANDLE_SIZE * 2 - 1,
                    HANDLE_SIZE * 2 - 1);
    cairo_stroke(cr);
  }

  char buf[64];
  snprintf(buf, sizeof buf, "%dx%d", n.w, n.h);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, buf, &ext);
  const int pad_x = 6, pad_y = 3, outside = HANDLE_SIZE + 6;
  double box_w = ext.width + pad_x * 2, box_h = ext.height + pad_y * 2;
  GdkRectangle label_mon = selection_label_monitor(n);
  int mon_l = label_mon.x - g_ov.virt.x;
  int mon_t = label_mon.y - g_ov.virt.y;
  int mon_r = mon_l + label_mon.width;
  int mon_b = mon_t + label_mon.height;
  double gbx = n.x - outside - box_w, gby = (double)n.y;
  if (gbx < mon_l) {
    gbx = n.x;
    gby = n.y - outside - box_h;
  }
  if (gby < mon_t) {
    gbx = n.x + 6;
    gby = n.y + 6;
  }
  if (gbx + box_w > mon_r)
    gbx = MAX((double)mon_l, mon_r - box_w);
  if (gby + box_h > mon_b)
    gby = MAX((double)mon_t, mon_b - box_h);
  double bx = gbx - off_x, by = gby - off_y;
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_rectangle(cr, bx, by, box_w, box_h);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);
  cairo_move_to(cr, bx + pad_x - ext.x_bearing, by + pad_y - ext.y_bearing);
  cairo_show_text(cr, buf);
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data) {
  (void)win;
  (void)user_data;
  close_overlay_and_quit();
  return TRUE;
}

static void request_close(GDBusConnection *bus, const char *path) {
  g_dbus_connection_call(bus, "org.freedesktop.portal.Desktop", path,
                         "org.freedesktop.portal.Request", "Close", NULL, NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static GdkMonitor *get_monitor_at_index(GdkDisplay *dsp, int idx) {
  GListModel *monitors = gdk_display_get_monitors(dsp);
  if (!monitors)
    return NULL;
  if (idx < 0 || (guint)idx >= g_list_model_get_n_items(monitors))
    return NULL;
  return GDK_MONITOR(g_list_model_get_item(monitors, (guint)idx));
}

static void portal_wait_free(PortalWait *pw) {
  if (!pw)
    return;
  if (pw->sub_id && pw->bus)
    g_dbus_connection_signal_unsubscribe(pw->bus, pw->sub_id);
  g_free(pw->request_path);
  g_clear_object(&pw->bus);
  g_free(pw);
}

static void virtual_monitor_bounds(GdkDisplay *dsp, GdkRectangle *out) {
  GListModel *monitors = gdk_display_get_monitors(dsp);
  int n = monitors ? (int)g_list_model_get_n_items(monitors) : 0;
  if (n <= 0) {
    out->x = out->y = 0;
    out->width = 1920;
    out->height = 1080;
    return;
  }
  int min_x = INT_MAX, min_y = INT_MAX, max_r = INT_MIN, max_b = INT_MIN;
  for (int i = 0; i < n; i++) {
    GdkMonitor *m = get_monitor_at_index(dsp, i);
    GdkRectangle g;
    gdk_monitor_get_geometry(m, &g);
    g_object_unref(m);
    min_x = MIN(min_x, g.x);
    min_y = MIN(min_y, g.y);
    max_r = MAX(max_r, g.x + g.width);
    max_b = MAX(max_b, g.y + g.height);
  }
  out->x = min_x;
  out->y = min_y;
  out->width = max_r - min_x;
  out->height = max_b - min_y;
}

static void open_overlay_from_pixbuf(GdkPixbuf *pix) {
  memset(&g_ov, 0, sizeof g_ov);
  g_ov.capture = g_object_ref(pix);
  g_ov.capture_surface = pixbuf_to_cairo_surface(pix);
  if (cairo_surface_status(g_ov.capture_surface) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "screenshot: failed to create cairo surface\n");
    cairo_surface_destroy(g_ov.capture_surface);
    g_ov.capture_surface = NULL;
    quit_capture_session();
    return;
  }
  g_ov.cap_w = gdk_pixbuf_get_width(pix);
  g_ov.cap_h = gdk_pixbuf_get_height(pix);

  GdkDisplay *dsp = gdk_display_get_default();
  virtual_monitor_bounds(dsp, &g_ov.virt);

  GListModel *monitors = gdk_display_get_monitors(dsp);
  int nmon = monitors ? (int)g_list_model_get_n_items(monitors) : 0;
  if (nmon <= 0)
    nmon = 1;
  g_owins = calloc((size_t)nmon, sizeof(*g_owins));
  if (!g_owins) {
    fprintf(stderr, "screenshot: out of memory creating overlay windows\n");
    quit_capture_session();
    return;
  }
  g_owin_count = nmon;

  for (int i = 0; i < nmon; i++) {
    OverlayWindow *ow = &g_owins[i];
    GdkMonitor *monitor = NULL;
    if (monitors && g_list_model_get_n_items(monitors) > 0) {
      monitor = get_monitor_at_index(dsp, i);
      gdk_monitor_get_geometry(monitor, &ow->geom);
    } else {
      ow->geom = g_ov.virt;
    }

    GtkWidget *win = gtk_window_new();
    ow->win = win;
    gtk_window_set_application(GTK_WINDOW(win), g_app);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), ow->geom.width, ow->geom.height);
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "screenshot");
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, 0);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, 0);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, 0);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    if (monitor)
      gtk_layer_set_monitor(GTK_WINDOW(win), monitor);
    if (monitor)
      g_object_unref(monitor);

    GtkWidget *da = gtk_drawing_area_new();
    ow->da = da;
    gtk_widget_set_focusable(da, TRUE);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), ow->geom.width);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), ow->geom.height);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_overlay, ow, NULL);
    gtk_window_set_child(GTK_WINDOW(win), da);
    g_object_set_data(G_OBJECT(da), "overlay-window", ow);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_widget_add_controller(da, GTK_EVENT_CONTROLLER(click));
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), da);
    g_signal_connect(click, "released", G_CALLBACK(on_click_released), da);

    GtkEventController *motion = gtk_event_controller_motion_new();
    gtk_widget_add_controller(da, motion);
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), da);

    GtkEventController *key = gtk_event_controller_key_new();
    gtk_widget_add_controller(win, key);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), NULL);

    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), NULL);
  }

  for (int i = 0; i < g_owin_count; i++)
    gtk_window_present(GTK_WINDOW(g_owins[i].win));

  set_cursor_all("crosshair");
  if (g_owin_count > 0 && g_owins[0].da)
    gtk_widget_grab_focus(g_owins[0].da);
}

static void on_portal_response(GDBusConnection *bus, const char *sender,
                               const char *path, const char *iface,
                               const char *signal, GVariant *params,
                               gpointer user_data) {
  (void)sender;
  (void)iface;
  (void)signal;
  PortalWait *pw = user_data;
  if (strcmp(path, pw->request_path) != 0)
    return;

  guint32 response = 0;
  GVariant *results = NULL;
  g_variant_get(params, "(u@a{sv})", &response, &results);

  g_dbus_connection_signal_unsubscribe(bus, pw->sub_id);
  pw->sub_id = 0;

  if (response != 0) {
    request_close(bus, path);
    portal_wait_free(pw);
    if (response != 1)
      fprintf(stderr, "screenshot: portal error code %u\n", response);
    quit_capture_session();
    g_variant_unref(results);
    return;
  }

  const char *uri = NULL;
  if (!g_variant_lookup(results, "uri", "&s", &uri) || !uri) {
    fprintf(stderr, "screenshot: portal response missing uri\n");
    g_variant_unref(results);
    request_close(bus, path);
    portal_wait_free(pw);
    quit_capture_session();
    return;
  }

  GError *err = NULL;
  gchar *file_path = g_filename_from_uri(uri, NULL, &err);
  g_variant_unref(results);
  request_close(bus, path);
  portal_wait_free(pw);

  if (!file_path) {
    fprintf(stderr, "screenshot: bad uri: %s\n", err ? err->message : "?");
    g_clear_error(&err);
    quit_capture_session();
    return;
  }

  GdkPixbuf *pb = gdk_pixbuf_new_from_file(file_path, &err);
  unlink(file_path);
  g_free(file_path);

  if (!pb) {
    fprintf(stderr, "screenshot: load image: %s\n", err ? err->message : "?");
    g_clear_error(&err);
    quit_capture_session();
    return;
  }

  open_overlay_from_pixbuf(pb);
  g_object_unref(pb);
}

static void portal_screenshot_cb(GObject *src, GAsyncResult *res,
                                 gpointer user_data) {
  GDBusConnection *bus = G_DBUS_CONNECTION(src);
  PortalWait *pw = user_data;
  GError *err = NULL;
  GVariant *reply = g_dbus_connection_call_finish(bus, res, &err);
  if (!reply) {
    fprintf(stderr, "screenshot: portal Screenshot call failed: %s\n",
            err->message);
    g_clear_error(&err);
    portal_wait_free(pw);
    quit_capture_session();
    return;
  }

  gchar *handle_path = NULL;
  g_variant_get(reply, "(o)", &handle_path);
  g_variant_unref(reply);
  if (!handle_path) {
    portal_wait_free(pw);
    quit_capture_session();
    return;
  }

  g_free(pw->request_path);
  pw->request_path = g_strdup(handle_path);
  pw->sub_id = g_dbus_connection_signal_subscribe(
      bus, NULL, "org.freedesktop.portal.Request", "Response", handle_path, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, on_portal_response, pw, NULL);
  g_free(handle_path);
}

static void begin_portal_screenshot(void) {
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    fprintf(stderr, "screenshot: session bus: %s\n", err->message);
    g_clear_error(&err);
    quit_capture_session();
    return;
  }

  char token[64];
  snprintf(token, sizeof token, "ss%u", (unsigned)g_random_int());

  GVariantDict opts;
  g_variant_dict_init(&opts, NULL);
  g_variant_dict_insert(&opts, "handle_token", "s", token);
  g_variant_dict_insert(&opts, "interactive", "b", FALSE);
  g_variant_dict_insert(&opts, "modal", "b", FALSE);

  PortalWait *pw = g_new0(PortalWait, 1);
  pw->bus = g_object_ref(bus);

  GVariant *options = g_variant_dict_end(&opts);
  GVariant *args = g_variant_new("(s@a{sv})", "", options);
  g_dbus_connection_call(
      bus, "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Screenshot",
      "Screenshot", args, G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
      NULL, portal_screenshot_cb, pw);
  g_object_unref(bus);
}

static gboolean idle_start_portal(gpointer user_data) {
  (void)user_data;
  begin_portal_screenshot();
  return G_SOURCE_REMOVE;
}

static void app_startup(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  g_app = app;
  g_application_hold(G_APPLICATION(app));
  g_idle_add(idle_start_portal, NULL);
}

int main(int argc, char **argv) {
  GtkApplication *app = gtk_application_new(
      "com.screenshot.app",
      (GApplicationFlags)(G_APPLICATION_NON_UNIQUE | G_APPLICATION_DEFAULT_FLAGS));
  g_signal_connect(app, "startup", G_CALLBACK(app_startup), NULL);
  int st = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return st;
}
