#include <stdio.h>  
#include <stdlib.h>  
#include <unistd.h>  
#include <fcntl.h>  
#include <signal.h>  
#include <sys/ioctl.h>  
#include <linux/joystick.h>
#include<X11/Xlib.h>
#include<X11/keysym.h>
#include<X11/extensions/XTest.h>

struct joy_event {
    int dx, dy;
    int rel_event;
    unsigned long buttons;
};

void interrupt_handler(int num);
int fd, fd_s;

int main(int argc, char **argv) {
    int oflags;
    fd = open("/dev/input/js0", O_RDWR);
    if (fd != -1) {
        signal(SIGIO, interrupt_handler);
        fcntl(fd, F_SETOWN, getpid());
        oflags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, oflags | FASYNC);

        while (1);

    }
    else
        printf("Could not open device!\n");
    return 0;
}

int simulate_mouse(struct joy_event *buf){
    int buttons = buf->buttons;
    int rel_event = buf->rel_event;

    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        return 0;
    }

    XEvent event;

    /* get info about current pointer position */
    XQueryPointer(dpy, RootWindow(dpy, DefaultScreen(dpy)),
            &event.xbutton.root, &event.xbutton.window,
            &event.xbutton.x_root, &event.xbutton.y_root,
            &event.xbutton.x, &event.xbutton.y,
            &event.xbutton.state);

    switch (buttons) {
        case 4:
            XTestFakeButtonEvent(dpy, 1, 1, 0);
            break;
        case 2:
            XTestFakeButtonEvent(dpy, 3, 1, 0);
            break;
        case 8:
            XTestFakeButtonEvent(dpy, 4, 1, 0);
            break;
        case 1:
            XTestFakeButtonEvent(dpy, 5, 1, 0);
            break;
	case 16:
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_Control_L), 1, 0);
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_C), 1, 0);
	    break;
	case 32:
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_Control_L), 1, 0);
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_V), 1, 0);
	    break;
	case 64:
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_KP_Enter), 1, 0);
	    break;
	case 128:
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_BackSpace), 1, 0);
	    break;
        default:
            XTestFakeButtonEvent(dpy, 1, 0, 0);
            XTestFakeButtonEvent(dpy, 3, 0, 0);
            XTestFakeButtonEvent(dpy, 4, 0, 0);
            XTestFakeButtonEvent(dpy, 5, 0, 0);
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_Control_L), 0, 0);
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_C), 0, 0);
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_V), 0, 0);
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_KP_Enter), 0, 0);
	    XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy,XK_BackSpace), 0, 0);
    }

    if(rel_event)	
	XTestFakeMotionEvent(dpy, -1, event.xbutton.x + buf->dx, event.xbutton.y + buf->dy, CurrentTime);
    XFlush(dpy);
    XCloseDisplay(dpy);
    return 1;
}
void interrupt_handler(int num) {
    struct joy_event buf;

    if (read(fd, &buf, sizeof (struct joy_event)) <= 0) printf("error\n");
    else {
        printf("%lu %d %d %d\n", buf.buttons, buf.dx, buf.dy, buf.rel_event);
        simulate_mouse(&buf);
    }
}
