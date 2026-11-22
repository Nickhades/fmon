#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    KeyCode k = XKeysymToKeycode(d, XK_Escape);
    XTestFakeKeyEvent(d, k, True, 0);
    XTestFakeKeyEvent(d, k, False, 0);
    XFlush(d);
    XCloseDisplay(d);
    return 0;
}