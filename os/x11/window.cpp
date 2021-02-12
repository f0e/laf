// LAF OS Library
// Copyright (C) 2018-2021  Igara Studio S.A.
// Copyright (C) 2017-2018  David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "os/x11/window.h"

#include "base/debug.h"
#include "base/string.h"
#include "base/thread.h"
#include "gfx/border.h"
#include "gfx/rect.h"
#include "gfx/region.h"
#include "os/event.h"
#include "os/event_queue.h"
#include "os/surface.h"
#include "os/system.h"
#include "os/window_spec.h"
#include "os/x11/keys.h"
#include "os/x11/screen.h"
#include "os/x11/x11.h"

#include <X11/cursorfont.h>
#include <map>

#define KEY_TRACE(...)
#define EVENT_TRACE(...)

#define LAF_X11_DOUBLE_CLICK_TIMEOUT 250

// TODO the window name should be customized from the CMakeLists.txt
//      properties (see OS_WND_CLASS_NAME too)
#define LAF_X11_WM_CLASS "Aseprite"

const int _NET_WM_STATE_REMOVE = 0;
const int _NET_WM_STATE_ADD    = 1;

const int _NET_WM_MOVERESIZE_SIZE_TOPLEFT      = 0;
const int _NET_WM_MOVERESIZE_SIZE_TOP          = 1;
const int _NET_WM_MOVERESIZE_SIZE_TOPRIGHT     = 2;
const int _NET_WM_MOVERESIZE_SIZE_RIGHT        = 3;
const int _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT  = 4;
const int _NET_WM_MOVERESIZE_SIZE_BOTTOM       = 5;
const int _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT   = 6;
const int _NET_WM_MOVERESIZE_SIZE_LEFT         = 7;
const int _NET_WM_MOVERESIZE_MOVE              = 8;
const int _NET_WM_MOVERESIZE_SIZE_KEYBOARD     = 9;
const int _NET_WM_MOVERESIZE_MOVE_KEYBOARD    = 10;
const int _NET_WM_MOVERESIZE_CANCEL           = 11;

namespace os {

namespace {

// Event generated by the window manager when the close button on the
// window is pressed by the userh.
Atom WM_DELETE_WINDOW = 0;

Atom _NET_FRAME_EXTENTS = 0;

Atom _NET_WM_STATE = 0;
Atom _NET_WM_STATE_MAXIMIZED_VERT;
Atom _NET_WM_STATE_MAXIMIZED_HORZ;

// Cursor Without pixels to simulate a hidden X11 cursor
Cursor empty_xcursor = None;

// See https://bugs.freedesktop.org/show_bug.cgi?id=12871 for more
// information, it looks like the official way to convert a X Window
// into our own user data pointer (WindowX11 instance) is using a map.
std::map<::Window, WindowX11*> g_activeWindows;

// Last time an XInput event was received, it's used to avoid
// processing mouse motion events that are generated at the same time
// for the XInput devices.
Time g_lastXInputEventTime = 0;

bool is_mouse_wheel_button(int button)
{
  return (button == Button4 || button == Button5 ||
          button == 6 || button == 7);
}

gfx::Point get_mouse_wheel_delta(int button)
{
  gfx::Point delta(0, 0);
  switch (button) {
    // Vertical wheel
    case Button4: delta.y = -1; break;
    case Button5: delta.y = +1; break;
    // Horizontal wheel
    case 6: delta.x = -1; break;
    case 7: delta.x = +1; break;
  }
  return delta;
}

} // anonymous namespace

// static
WindowX11* WindowX11::getPointerFromHandle(::Window handle)
{
  auto it = g_activeWindows.find(handle);
  if (it != g_activeWindows.end())
    return it->second;
  else
    return nullptr;
}

// static
void WindowX11::addWindow(WindowX11* window)
{
  ASSERT(g_activeWindows.find(window->x11window()) == g_activeWindows.end());
  g_activeWindows[window->x11window()] = window;
}

// static
void WindowX11::removeWindow(WindowX11* window)
{
  auto it = g_activeWindows.find(window->x11window());
  ASSERT(it != g_activeWindows.end());
  if (it != g_activeWindows.end()) {
    ASSERT(it->second == window);
    g_activeWindows.erase(it);
  }
}

WindowX11::WindowX11(::Display* display, const WindowSpec& spec)
  : m_display(display)
  , m_gc(nullptr)
  , m_cursor(None)
  , m_xcursorImage(nullptr)
  , m_xic(nullptr)
  , m_scale(spec.scale())
  , m_lastMousePos(-1, -1)
  , m_lastClientSize(0, 0)
  , m_doubleClickButton(Event::NoneButton)
  , m_borderless(spec.borderless())
{
  // Cache some atoms (TODO improve this to cache more atoms)
  if (!_NET_FRAME_EXTENTS)
    _NET_FRAME_EXTENTS = XInternAtom(m_display, "_NET_FRAME_EXTENTS", False);
  if (!_NET_WM_STATE) {
    _NET_WM_STATE = XInternAtom(m_display, "_NET_WM_STATE", False);
    _NET_WM_STATE_MAXIMIZED_VERT = XInternAtom(m_display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    _NET_WM_STATE_MAXIMIZED_HORZ = XInternAtom(m_display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
  }

  // Initialize special messages (just the first time a WindowX11 is
  // created)
  if (!WM_DELETE_WINDOW)
    WM_DELETE_WINDOW = XInternAtom(m_display, "WM_DELETE_WINDOW", False);

  ::Window root = XDefaultRootWindow(m_display);

  XSetWindowAttributes swa;
  swa.event_mask = (StructureNotifyMask | ExposureMask | PropertyChangeMask |
                    EnterWindowMask | LeaveWindowMask | FocusChangeMask |
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                    KeyPressMask | KeyReleaseMask);

  // We cannot use the override-redirect state because it removes too
  // much behavior of the WM (cannot resize the custom frame as other
  // regular windows in the WM, etc.)
  //swa.override_redirect = (spec.borderless() ? True: False);

  gfx::Rect rc;

  if (!spec.frame().isEmpty()) {
    rc = spec.frame();
    m_initializingFromFrame = true;
  }
  else
    rc = spec.contentRect();

  m_window = XCreateWindow(
    m_display, root,
    rc.x, rc.y, rc.w, rc.h, 0,
    CopyFromParent,
    InputOutput,
    CopyFromParent,
    CWEventMask, // Do not use CWOverrideRedirect
    &swa);

  if (!m_window)
    throw std::runtime_error("Cannot create X11 window");

  setWMClass(LAF_X11_WM_CLASS);

  // Special frame for this window
  if (spec.floating()) {
    // We use _NET_WM_WINDOW_TYPE_UTILITY for floating windows
    Atom _NET_WM_WINDOW_TYPE = XInternAtom(m_display, "_NET_WM_WINDOW_TYPE", False);
    Atom _NET_WM_WINDOW_TYPE_UTILITY = XInternAtom(m_display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    Atom _NET_WM_WINDOW_TYPE_NORMAL = XInternAtom(m_display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    if (_NET_WM_WINDOW_TYPE &&
        _NET_WM_WINDOW_TYPE_UTILITY &&
        _NET_WM_WINDOW_TYPE_NORMAL) {
      // We've to specify the window types in order of preference (but
      // must include at least one of the basic window type atoms).
      std::vector<Atom> data = { _NET_WM_WINDOW_TYPE_UTILITY,
                                 _NET_WM_WINDOW_TYPE_NORMAL };
      XChangeProperty(
        m_display, m_window, _NET_WM_WINDOW_TYPE,
        XA_ATOM, 32, PropModeReplace,
        (const unsigned char*)&data[0], data.size());
    }
  }

  // To remove the borders and keep the window behavior of the WM
  // (e.g. Super key + mouse to resize/move the window), we can use
  // this trick setting the _MOTIF_WM_HINTS flag to 2.
  //
  // The alternatives (using _NET_WM_WINDOW_TYPE or override-redirect)
  // are useless because they remove the default behavior of the
  // operating system (making a complete "naked" window without
  // behavior at all).
  if (spec.borderless()) {
    std::vector<uint32_t> data = { 2 };
    Atom _MOTIF_WM_HINTS = XInternAtom(m_display, "_MOTIF_WM_HINTS", False);
    XChangeProperty(
      m_display, m_window, _MOTIF_WM_HINTS,
      XA_CARDINAL, 32, PropModeReplace,
      (const unsigned char*)&data[0], data.size());
  }

  // Receive stylus/eraser events
  X11::instance()->xinput().selectExtensionEvents(m_display, m_window);

  // Change preferred origin/size for the window (this should be used by the WM)
  XSizeHints* hints = XAllocSizeHints();
  hints->flags  =
    PPosition | PSize |
    PResizeInc | PWinGravity;
  hints->x = rc.x;
  hints->y = rc.y;
  hints->width  = rc.w;
  hints->height = rc.h;
  hints->width_inc = 4;
  hints->height_inc = 4;
  hints->win_gravity = SouthGravity;
  XSetWMNormalHints(m_display, m_window, hints);
  XFree(hints);

  XMapWindow(m_display, m_window);
  XSetWMProtocols(m_display, m_window, &WM_DELETE_WINDOW, 1);

  if (spec.floating() && spec.parent()) {
    ASSERT(static_cast<WindowX11*>(spec.parent())->m_window);
    XSetTransientForHint(
      m_display,
      m_window,
      static_cast<WindowX11*>(spec.parent())->m_window);
  }

  m_gc = XCreateGC(m_display, m_window, 0, nullptr);

  XIM xim = X11::instance()->xim();
  if (xim) {
    m_xic = XCreateIC(xim,
                      XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                      XNClientWindow, m_window,
                      XNFocusWindow, m_window,
                      nullptr);
  }

  WindowX11::addWindow(this);
}

WindowX11::~WindowX11()
{
  if (m_xcursorImage != None)
    XcursorImageDestroy(m_xcursorImage);
  if (m_xic)
    XDestroyIC(m_xic);
  XFreeGC(m_display, m_gc);
  XDestroyWindow(m_display, m_window);

  WindowX11::removeWindow(this);
}

void WindowX11::queueEvent(Event& ev)
{
  onQueueEvent(ev);
}

os::ScreenRef WindowX11::screen() const
{
  return os::make_ref<ScreenX11>(DefaultScreen(m_display));
}

os::ColorSpaceRef WindowX11::colorSpace() const
{
  // TODO get the window color space
  return os::instance()->makeColorSpace(gfx::ColorSpace::MakeSRGB());
}

void WindowX11::setScale(const int scale)
{
  m_scale = scale;
  onResize(clientSize());
}

bool WindowX11::isVisible() const
{
  // TODO
  return true;
}

void WindowX11::setVisible(bool visible)
{
  // TODO
}

void WindowX11::activate()
{
  Atom _NET_ACTIVE_WINDOW = XInternAtom(m_display, "_NET_ACTIVE_WINDOW", False);
  if (!_NET_ACTIVE_WINDOW)
    return;                     // No atoms?

  ::Window root = XDefaultRootWindow(m_display);
  XEvent event;
  memset(&event, 0, sizeof(event));
  event.xany.type = ClientMessage;
  event.xclient.window = m_window;
  event.xclient.message_type = _NET_ACTIVE_WINDOW;
  event.xclient.format = 32;
  event.xclient.data.l[0] = 1; // 1 when the request comes from an application
  event.xclient.data.l[1] = CurrentTime;
  event.xclient.data.l[2] = 0;
  event.xclient.data.l[3] = 0;

  XSendEvent(m_display, root, 0,
             SubstructureNotifyMask | SubstructureRedirectMask, &event);
}

void WindowX11::maximize()
{
  ::Window root = XDefaultRootWindow(m_display);
  XEvent event;
  memset(&event, 0, sizeof(event));
  event.xany.type = ClientMessage;
  event.xclient.window = m_window;
  event.xclient.message_type = _NET_WM_STATE;
  event.xclient.format = 32;
  event.xclient.data.l[0] = (isMaximized() ? _NET_WM_STATE_REMOVE:
                                             _NET_WM_STATE_ADD);
  event.xclient.data.l[1] = _NET_WM_STATE_MAXIMIZED_VERT;
  event.xclient.data.l[2] = _NET_WM_STATE_MAXIMIZED_HORZ;

  XSendEvent(m_display, root, 0,
             SubstructureNotifyMask | SubstructureRedirectMask, &event);
}

void WindowX11::minimize()
{
  XIconifyWindow(m_display, m_window, DefaultScreen(m_display));
}

bool WindowX11::isMaximized() const
{
  bool result = false;
  Atom actual_type;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  Atom* prop = nullptr;
  int res = XGetWindowProperty(m_display, m_window,
                               _NET_WM_STATE,
                               0, 4,
                               False, XA_ATOM,
                               &actual_type, &actual_format,
                               &nitems, &bytes_after,
                               (unsigned char**)&prop);

  if (res == Success) {
    for (int i=0; i<nitems; ++i) {
      if (prop[i] == _NET_WM_STATE_MAXIMIZED_VERT ||
          prop[i] == _NET_WM_STATE_MAXIMIZED_HORZ) {
        result = true;
      }
    }
    XFree(prop);
  }
  return result;
}

bool WindowX11::isMinimized() const
{
  return false;
}

bool WindowX11::isFullscreen() const
{
  // TODO ask _NET_WM_STATE_FULLSCREEN atom in _NET_WM_STATE window property
  return m_fullScreen;
}

void WindowX11::setFullscreen(bool state)
{
  if (isFullscreen() == state)
    return;

  Atom _NET_WM_STATE_FULLSCREEN = XInternAtom(m_display, "_NET_WM_STATE_FULLSCREEN", False);
  if (!_NET_WM_STATE || !_NET_WM_STATE_FULLSCREEN)
    return;                     // No atoms?

  // From _NET_WM_STATE section in https://specifications.freedesktop.org/wm-spec/1.3/ar01s05.html#idm46018259875952
  //
  //   "Client wishing to change the state of a window MUST send a
  //    _NET_WM_STATE client message to the root window. The Window
  //    Manager MUST keep this property updated to reflect the
  //    current state of the window."
  //
  ::Window root = XDefaultRootWindow(m_display);
  XEvent event;
  memset(&event, 0, sizeof(event));
  event.xany.type = ClientMessage;
  event.xclient.window = m_window;
  event.xclient.message_type = _NET_WM_STATE;
  event.xclient.format = 32;
  // The action
  event.xclient.data.l[0] = (state ? _NET_WM_STATE_ADD:
                                     _NET_WM_STATE_REMOVE);
  event.xclient.data.l[1] = _NET_WM_STATE_FULLSCREEN; // First property to alter
  event.xclient.data.l[2] = 0;      // Second property to alter
  event.xclient.data.l[3] = 0;      // Source indication

  XSendEvent(m_display, root, 0,
             SubstructureNotifyMask | SubstructureRedirectMask, &event);

  m_fullScreen = state;
}

void WindowX11::setTitle(const std::string& title)
{
  XTextProperty prop;
  prop.value = (unsigned char*)title.c_str();
  prop.encoding = XA_STRING;
  prop.format = 8;
  prop.nitems = std::strlen((char*)title.c_str());
  XSetWMName(m_display, m_window, &prop);
}

void WindowX11::setIcons(const SurfaceList& icons)
{
  if (!m_display || !m_window)
    return;

  bool first = true;
  for (auto& icon : icons) {
    const int w = icon->width();
    const int h = icon->height();

    SurfaceFormatData format;
    icon->getFormat(&format);

    std::vector<unsigned long> data(w*h+2);
    int i = 0;
    data[i++] = w;
    data[i++] = h;
    for (int y=0; y<h; ++y) {
      const uint32_t* p = (const uint32_t*)icon->getData(0, y);
      for (int x=0; x<w; ++x, ++p) {
        uint32_t c = *p;
        data[i++] =
          (((c & format.blueMask ) >> format.blueShift )      ) |
          (((c & format.greenMask) >> format.greenShift) <<  8) |
          (((c & format.redMask  ) >> format.redShift  ) << 16) |
          (((c & format.alphaMask) >> format.alphaShift) << 24);
      }
    }

    Atom _NET_WM_ICON = XInternAtom(m_display, "_NET_WM_ICON", False);
    XChangeProperty(
      m_display, m_window, _NET_WM_ICON, XA_CARDINAL, 32,
      first ? PropModeReplace:
              PropModeAppend,
      (const unsigned char*)&data[0], data.size());

    first = false;
  }
}

gfx::Rect WindowX11::frame() const
{
  gfx::Rect rc = contentRect();
  rc.enlarge(m_frameExtents);
  return rc;
}

gfx::Rect WindowX11::contentRect() const
{
  ::Window root;
  int x, y;
  unsigned int width, height, border, depth;
  XGetGeometry(m_display, m_window, &root,
               &x, &y, &width, &height, &border, &depth);

  ::Window child_return;
  XTranslateCoordinates(m_display, m_window, root,
                        0, 0, &x, &y, &child_return);

  return gfx::Rect(x, y, int(width), int(height));
}

std::string WindowX11::title() const
{
  XTextProperty prop;
  if (!XGetWMName(m_display, m_window, &prop) || !prop.value)
    return std::string();

  std::string value = (const char*)prop.value;
  XFree(prop.value);
  return value;
}

gfx::Size WindowX11::clientSize() const
{
  ::Window root;
  int x, y;
  unsigned int width, height, border, depth;
  XGetGeometry(m_display, m_window, &root,
               &x, &y, &width, &height, &border, &depth);
  return gfx::Size(int(width), int(height));
}

gfx::Size WindowX11::restoredSize() const
{
  ::Window root;
  int x, y;
  unsigned int width, height, border, depth;
  XGetGeometry(m_display, m_window, &root,
               &x, &y, &width, &height, &border, &depth);
  return gfx::Size(int(width), int(height));
}

void WindowX11::captureMouse()
{
  XGrabPointer(m_display, m_window, False,
               PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
               GrabModeAsync, GrabModeAsync,
               None, None, CurrentTime);
}

void WindowX11::releaseMouse()
{
  XUngrabPointer(m_display, CurrentTime);
}

void WindowX11::setMousePosition(const gfx::Point& position)
{
  ::Window root;
  int x, y;
  unsigned int w, h, border, depth;
  XGetGeometry(m_display, m_window, &root,
               &x, &y, &w, &h, &border, &depth);
  XWarpPointer(m_display, m_window, m_window, 0, 0, w, h,
               position.x*m_scale, position.y*m_scale);
}

void WindowX11::invalidateRegion(const gfx::Region& rgn)
{
  gfx::Rect bounds = rgn.bounds();
  onPaint(gfx::Rect(bounds.x*m_scale,
                    bounds.y*m_scale,
                    bounds.w*m_scale,
                    bounds.h*m_scale));
}

bool WindowX11::setNativeMouseCursor(NativeCursor cursor)
{
  Cursor xcursor = None;

  switch (cursor) {
    case NativeCursor::Hidden: {
      if (empty_xcursor == None) {
        char data = 0;
        Pixmap image = XCreateBitmapFromData(
          m_display, m_window, (char*)&data, 1, 1);

        XColor color;
        empty_xcursor = XCreatePixmapCursor(
          m_display, image, image, &color, &color, 0, 0);

        XFreePixmap(m_display, image);
      }
      xcursor = empty_xcursor;
      break;
    }
    case NativeCursor::Arrow:
      xcursor = XCreateFontCursor(m_display, XC_arrow);
      break;
    case NativeCursor::Crosshair:
      xcursor = XCreateFontCursor(m_display, XC_crosshair);
      break;
    case NativeCursor::IBeam:
      xcursor = XCreateFontCursor(m_display, XC_xterm);
      break;
    case NativeCursor::Wait:
      xcursor = XCreateFontCursor(m_display, XC_watch);
      break;
    case NativeCursor::Link:
      xcursor = XCreateFontCursor(m_display, XC_hand1);
      break;
    case NativeCursor::Help:
      xcursor = XCreateFontCursor(m_display, XC_question_arrow);
      break;
    case NativeCursor::Forbidden:
      xcursor = XCreateFontCursor(m_display, XC_X_cursor);
      break;
    case NativeCursor::Move:
      xcursor = XCreateFontCursor(m_display, XC_fleur);
      break;
    case NativeCursor::SizeN:
      xcursor = XCreateFontCursor(m_display, XC_top_side);
      break;
    case NativeCursor::SizeNS:
      xcursor = XCreateFontCursor(m_display, XC_sb_v_double_arrow);
      break;
    case NativeCursor::SizeS:
      xcursor = XCreateFontCursor(m_display, XC_bottom_side);
      break;
    case NativeCursor::SizeW:
      xcursor = XCreateFontCursor(m_display, XC_left_side);
      break;
    case NativeCursor::SizeE:
      xcursor = XCreateFontCursor(m_display, XC_right_side);
      break;
    case NativeCursor::SizeWE:
      xcursor = XCreateFontCursor(m_display, XC_sb_h_double_arrow);
      break;
    case NativeCursor::SizeNW:
      xcursor = XCreateFontCursor(m_display, XC_top_left_corner);
      break;
    case NativeCursor::SizeNE:
      xcursor = XCreateFontCursor(m_display, XC_top_right_corner);
      break;
    case NativeCursor::SizeSW:
      xcursor = XCreateFontCursor(m_display, XC_bottom_left_corner);
      break;
    case NativeCursor::SizeSE:
      xcursor = XCreateFontCursor(m_display, XC_bottom_right_corner);
      break;
  }

  return setX11Cursor(xcursor);
}

bool WindowX11::setNativeMouseCursor(const os::Surface* surface,
                                     const gfx::Point& focus,
                                     const int scale)
{
  ASSERT(surface);

  // This X11 server doesn't support ARGB cursors.
  if (!XcursorSupportsARGB(m_display))
    return false;

  SurfaceFormatData format;
  surface->getFormat(&format);

  // Only for 32bpp surfaces
  if (format.bitsPerPixel != 32)
    return false;

  const int w = scale*surface->width();
  const int h = scale*surface->height();

  Cursor xcursor = None;
  if (m_xcursorImage == None ||
      m_xcursorImage->width != XcursorDim(w) ||
      m_xcursorImage->height != XcursorDim(h)) {
    if (m_xcursorImage != None)
      XcursorImageDestroy(m_xcursorImage);
    m_xcursorImage = XcursorImageCreate(w, h);
  }
  if (m_xcursorImage != None) {
    XcursorPixel* dst = m_xcursorImage->pixels;
    for (int y=0; y<h; ++y) {
      const uint32_t* src = (const uint32_t*)surface->getData(0, y/scale);
      for (int x=0, u=0; x<w; ++x, ++dst) {
        uint32_t c = *src;
        *dst =
          (((c & format.alphaMask) >> format.alphaShift) << 24) |
          (((c & format.redMask  ) >> format.redShift  ) << 16) |
          (((c & format.greenMask) >> format.greenShift) << 8) |
          (((c & format.blueMask ) >> format.blueShift ));
        if (++u == scale) {
          u = 0;
          ++src;
        }
      }
    }

    m_xcursorImage->xhot = scale*focus.x + scale/2;
    m_xcursorImage->yhot = scale*focus.y + scale/2;
    xcursor = XcursorImageLoadCursor(m_display,
                                     m_xcursorImage);
  }

  return setX11Cursor(xcursor);
}

void WindowX11::performWindowAction(const WindowAction action,
                                    const Event* ev)
{
  Atom _NET_WM_MOVERESIZE = XInternAtom(m_display, "_NET_WM_MOVERESIZE", False);
  if (!_NET_WM_MOVERESIZE)
    return;                     // No atoms?

  int x = (ev ? ev->position().x: 0);
  int y = (ev ? ev->position().y: 0);
  int button = (ev ? get_x_mouse_button_from_event(ev->button()): 0);
  Atom direction = 0;
  switch (action) {
    case WindowAction::Cancel:                direction = _NET_WM_MOVERESIZE_CANCEL; break;
    case WindowAction::Move:                  direction = _NET_WM_MOVERESIZE_MOVE; break;
    case WindowAction::ResizeFromTopLeft:     direction = _NET_WM_MOVERESIZE_SIZE_TOPLEFT; break;
    case WindowAction::ResizeFromTop:         direction = _NET_WM_MOVERESIZE_SIZE_TOP; break;
    case WindowAction::ResizeFromTopRight:    direction = _NET_WM_MOVERESIZE_SIZE_TOPRIGHT; break;
    case WindowAction::ResizeFromLeft:        direction = _NET_WM_MOVERESIZE_SIZE_LEFT; break;
    case WindowAction::ResizeFromRight:       direction = _NET_WM_MOVERESIZE_SIZE_RIGHT; break;
    case WindowAction::ResizeFromBottomLeft:  direction = _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT; break;
    case WindowAction::ResizeFromBottom:      direction = _NET_WM_MOVERESIZE_SIZE_BOTTOM; break;
    case WindowAction::ResizeFromBottomRight: direction = _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT; break;
  }

  // From:
  // https://specifications.freedesktop.org/wm-spec/latest/ar01s04.html#idm46075117309248
  // "The Client MUST release all grabs prior to sending such
  //  message (except for the _NET_WM_MOVERESIZE_CANCEL message)."
  if (direction != _NET_WM_MOVERESIZE_CANCEL)
    releaseMouse();

  ::Window root = XDefaultRootWindow(m_display);
  ::Window child_return;
  XTranslateCoordinates(m_display, m_window, root,
                        x, y, &x, &y, &child_return);

  XEvent event;
  memset(&event, 0, sizeof(event));
  event.xany.type = ClientMessage;
  event.xclient.window = m_window;
  event.xclient.message_type = _NET_WM_MOVERESIZE;
  event.xclient.format = 32;
  event.xclient.data.l[0] = x;
  event.xclient.data.l[1] = y;
  event.xclient.data.l[2] = direction;
  event.xclient.data.l[3] = button;
  event.xclient.data.l[4] = 0;

  XSendEvent(m_display, root, 0,
             SubstructureNotifyMask | SubstructureRedirectMask, &event);
}

void WindowX11::setWMClass(const std::string& res_class)
{
  std::string res_name = base::string_to_lower(res_class);
  XClassHint ch;
  ch.res_name = (char*)res_name.c_str();
  ch.res_class = (char*)res_class.c_str();
  XSetClassHint(m_display, m_window, &ch);
}

bool WindowX11::setX11Cursor(::Cursor xcursor)
{
  if (m_cursor != None) {
    if (m_cursor != empty_xcursor) // Don't delete empty_xcursor
      XFreeCursor(m_display, m_cursor);
    m_cursor = None;
  }
  if (xcursor != None) {
    m_cursor = xcursor;
    XDefineCursor(m_display, m_window, xcursor);
    return true;
  }
  else
    return false;
}

void WindowX11::processX11Event(XEvent& event)
{
  auto xinput = &X11::instance()->xinput();
  if (xinput->handleExtensionEvent(event)) {
    Event ev;
    xinput->convertExtensionEvent(event, ev, m_scale,
                                  g_lastXInputEventTime);
    queueEvent(ev);
    return;
  }

  switch (event.type) {

    case ConfigureNotify: {
      gfx::Rect rc(event.xconfigure.x,
                   event.xconfigure.y,
                   event.xconfigure.width,
                   event.xconfigure.height);

      if (m_initializingFromFrame) {
        m_initializingFromFrame = false;

        rc.w -= m_frameExtents.width();
        rc.h -= m_frameExtents.height();
        rc.h += 4; // TODO it's one unit of PResizeInc, try to get this value in other way

        XResizeWindow(m_display, m_window, rc.w, rc.h);
        return;
      }

      if (rc.w > 0 && rc.h > 0 && rc.size() != m_lastClientSize) {
        m_lastClientSize = rc.size();
        onResize(rc.size());
      }
      break;
    }

    case Expose: {
      gfx::Rect rc(event.xexpose.x, event.xexpose.y,
                   event.xexpose.width, event.xexpose.height);
      onPaint(rc);
      break;
    }

    case KeyPress:
    case KeyRelease: {
      Event ev;
      ev.setType(event.type == KeyPress ? Event::KeyDown: Event::KeyUp);

      KeySym keysym = XLookupKeysym(&event.xkey, 0);
      ev.setScancode(x11_keysym_to_scancode(keysym));

      if (m_xic) {
        std::vector<char> buf(16);
        size_t len = Xutf8LookupString(m_xic, &event.xkey,
                                       &buf[0], buf.size(),
                                       nullptr, nullptr);
        if (len < buf.size())
          buf[len] = 0;
        std::wstring wideChars = base::from_utf8(std::string(&buf[0]));
        if (!wideChars.empty())
          ev.setUnicodeChar(wideChars[0]);
        KEY_TRACE("Xutf8LookupString %s\n", &buf[0]);
      }

      // Key event used by the input method (e.g. when the user
      // presses a dead key).
      if (XFilterEvent(&event, m_window))
        break;

      int modifiers = (int)get_modifiers_from_x(event.xkey.state);
      switch (keysym) {
        case XK_space: {
          switch (event.type) {
            case KeyPress:
              g_spaceBarIsPressed = true;
              break;
            case KeyRelease:
              g_spaceBarIsPressed = false;

              // If the next event after a KeyRelease is a KeyPress of
              // the same keycode (the space bar in this case), it
              // means that this KeyRelease is just a repetition of a
              // the same keycode.
              if (XEventsQueued(m_display, QueuedAfterReading)) {
                XEvent nextEvent;
                XPeekEvent(m_display, &nextEvent);
                if (nextEvent.type == KeyPress &&
                    nextEvent.xkey.time == event.xkey.time &&
                    nextEvent.xkey.keycode == event.xkey.keycode) {
                  g_spaceBarIsPressed = true;
                }
              }
              break;
          }
          break;
        }
        case XK_Shift_L:
        case XK_Shift_R:
          modifiers |= kKeyShiftModifier;
          break;
        case XK_Control_L:
        case XK_Control_R:
          modifiers |= kKeyCtrlModifier;
          break;
        case XK_Alt_L:
        case XK_Alt_R:
          modifiers |= kKeyAltModifier;
          break;
        case XK_Meta_L:
        case XK_Super_L:
        case XK_Meta_R:
        case XK_Super_R:
          modifiers |= kKeyWinModifier;
          break;
      }
      ev.setModifiers((KeyModifiers)modifiers);
      KEY_TRACE("%s state=%04x keycode=%04x\n",
                (event.type == KeyPress ? "KeyPress": "KeyRelease"),
                event.xkey.state,
                event.xkey.keycode);
      KEY_TRACE(" > %s\n", XKeysymToString(keysym));

      queueEvent(ev);
      break;
    }

    case ButtonPress:
    case ButtonRelease: {
      // This can happen when the button press/release events are
      // handled in XInput
      if (event.xmotion.time == g_lastXInputEventTime)
        break;

      Event ev;
      if (is_mouse_wheel_button(event.xbutton.button)) {
        if (event.type == ButtonPress) {
          ev.setType(Event::MouseWheel);
          ev.setWheelDelta(get_mouse_wheel_delta(event.xbutton.button));
        }
        else {
          // Ignore ButtonRelese for the mouse wheel to avoid
          // duplicating MouseWheel event effects.
          break;
        }
      }
      else {
        ev.setType(event.type == ButtonPress ? Event::MouseDown:
                                               Event::MouseUp);

        Event::MouseButton button =
          get_mouse_button_from_x(event.xbutton.button);
        ev.setButton(button);

        if (event.type == ButtonPress) {
          if (m_doubleClickButton == button &&
              base::current_tick() - m_doubleClickTick < LAF_X11_DOUBLE_CLICK_TIMEOUT) {
            ev.setType(Event::MouseDoubleClick);
            m_doubleClickButton = Event::NoneButton;
          }
          else {
            m_doubleClickButton = button;
            m_doubleClickTick = base::current_tick();
          }
        }
      }
      ev.setModifiers(get_modifiers_from_x(event.xbutton.state));
      ev.setPosition(gfx::Point(event.xbutton.x / m_scale,
                                event.xbutton.y / m_scale));

      queueEvent(ev);
      break;
    }

    case MotionNotify: {
      // This can happen when the motion event are handled in XInput
      if (event.xmotion.time == g_lastXInputEventTime)
        break;

      // Reset double-click state
      m_doubleClickButton = Event::NoneButton;

      gfx::Point pos(event.xmotion.x / m_scale,
                     event.xmotion.y / m_scale);

      if (m_lastMousePos == pos)
        break;
      m_lastMousePos = pos;

      Event ev;
      ev.setType(Event::MouseMove);
      ev.setModifiers(get_modifiers_from_x(event.xmotion.state));
      ev.setPosition(pos);
      queueEvent(ev);
      break;
    }

    case EnterNotify:
    case LeaveNotify:
      g_spaceBarIsPressed = false;

      // "mode" can be NotifyGrab or NotifyUngrab when middle mouse
      // button is pressed/released. We must not generated
      // MouseEnter/Leave events on those cases, only on NotifyNormal
      // (when mouse leaves/enter the X11 window).
      if (event.xcrossing.mode == NotifyNormal) {
        Event ev;
        ev.setType(event.type == EnterNotify ? Event::MouseEnter:
                                               Event::MouseLeave);
        ev.setModifiers(get_modifiers_from_x(event.xcrossing.state));
        ev.setPosition(gfx::Point(event.xcrossing.x / m_scale,
                                  event.xcrossing.y / m_scale));
        queueEvent(ev);
      }
      break;

    case ClientMessage:
      // When the close button is pressed
      if (Atom(event.xclient.data.l[0]) == WM_DELETE_WINDOW) {
        Event ev;
        ev.setType(Event::CloseWindow);
        queueEvent(ev);
      }
      break;

    case PropertyNotify:
      if (event.xproperty.atom == _NET_FRAME_EXTENTS) {
        if (m_borderless) {
          std::vector<unsigned long> data(4, 0);
          XChangeProperty(
            m_display, m_window, _NET_FRAME_EXTENTS, XA_CARDINAL, 32,
            PropModeReplace, (const unsigned char*)&data[0], data.size());
        }

        Atom actual_type;
        int actual_format;
        unsigned long nitems;
        unsigned long bytes_after;
        unsigned long* prop = nullptr;
        int res = XGetWindowProperty(m_display, m_window,
                                     _NET_FRAME_EXTENTS,
                                     0, 4,
                                     False, XA_CARDINAL,
                                     &actual_type, &actual_format,
                                     &nitems, &bytes_after,
                                     (unsigned char**)&prop);

        if (res == Success && nitems == 4) {
          // Get the dimension of the title bar + borders (WM decorators)
          m_frameExtents.left(prop[0]);
          m_frameExtents.right(prop[1]);
          m_frameExtents.top(prop[2]);
          m_frameExtents.bottom(prop[3]);
          XFree(prop);
        }
      }
      break;
  }
}

} // namespace os
