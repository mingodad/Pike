/* _Xlib.pmod
 *
 * kluge
 */

object display_re = Regexp("([^:]*):([0-9]+).([0-9]+)");

array(string) window_attributes =
({ "background_pixmap",
   "background_pixel",
   "border_pixmap",
   "border_pixel",
   "bit_gravity",
   "win_gravity",
   "backing_store",
   "backing_bit_planes",
   "backing_pixel",
   "override_redirect",
   "save_under",
   "event_mask",
   "do_not_propagate_mask",
   "colormap",
   "cursor" });

mapping(string:int) event_masks =
([
  "KeyPress" : 1<<0,
  "KeyRelease" : 1<<1,
  "ButtonPress" : 1<<2,
  "ButtonRelease" : 1<<3,
  "EnterWindow" : 1<<4,
  "LeaveWindow" : 1<<5,
  "PointerMotion" : 1<<6,
  "PointerMotionHint" : 1<<7,
  "Button1Motion" : 1<<8,
  "Button2Motion" : 1<<9,
  "Button3Motion" : 1<<10,
  "Button4Motion" : 1<<11,
  "Button5Motion" : 1<<12,
  "ButtonMotion" : 1<<13,
  "KeymapState" : 1<<14,
  "Exposure" : 1<<15,
  "VisibilityChange" : 1<<16,
  "StructureNotify" : 1<<17,
  "ResizeRedirect" : 1<<18,
  "SubstructureNotify" : 1<<19,
  "SubstructureRedirect" : 1<<20,
  "FocusChange" : 1<<21,
  "PropertyChange" : 1<<22,
  "ColormapChange" : 1<<23,
  "OwnerGrabButton" : 1<<24
 ]);

array(string) event_types =
({
  "Error",
  "Reply",
  "KeyPress",
  "KeyRelease",
  "ButtonPress",
  "ButtonRelease",
  "MotionNotify",
  "EnterNotify",
  "LeaveNotify",
  "FocusIn",
  "FocusOut",
  "KeymapNotify",
  "Expose",
  "GraphicsExpose",
  "NoExpose",
  "VisibilityNotify",
  "CreateNotify",
  "DestroyNotify",
  "UnmapNotify",
  "MapNotify",
  "MapRequest",
  "ReparentNotify",
  "ConfigureNotify",
  "ConfigureRequest",
  "GravityNotify",
  "ResizeRequest",
  "CirculateNotify",
  "CirculateRequest",
  "PropertyNotify",
  "SelectionClear",
  "SelectionRequest",
  "SelectionNotify",
  "ColormapNotify",
  "ClientMessage",
  "MappingNotify" });
