/* /dev/wrld/window: a desktop window for Linux programs on THE WRLD OS.
 *
 *   int fd = open("/dev/wrld/window", O_RDWR | O_CLOEXEC);
 *   struct wrldwin_create c = { 640, 480, WRLDWIN_RESIZABLE, "Title" };
 *   ioctl(fd, WRLDWIN_CREATE, &c);
 *   uint32_t *px = mmap(NULL, 640 * 480 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
 *   ... draw 0xAARRGGBB pixels (Cairo's CAIRO_FORMAT_ARGB32, stride width * 4) ...
 *   ioctl(fd, WRLDWIN_PRESENT, 0);
 *   struct wrldwin_event ev[16]; read(fd, ev, sizeof ev);   (poll/epoll work too)
 *
 * After a WRLDWIN_EV_RESIZE, call WRLDWIN_RESIZE with the new size and mmap again.
 * read() returns 0 once the window is gone. */
#ifndef WRLDWIN_H
#define WRLDWIN_H
#include <stdint.h>

#define WRLDWIN_CREATE  0x57A1
#define WRLDWIN_PRESENT 0x57A2
#define WRLDWIN_RESIZE  0x57A3
#define WRLDWIN_TITLE   0x57A4
#define WRLDWIN_INFO    0x57A5
#define WRLDWIN_RESIZABLE 1

struct wrldwin_create { int32_t width, height; uint32_t flags; char title[100]; };
struct wrldwin_size { int32_t width, height; };
struct wrldwin_info { int32_t width, height, buf_width, buf_height; };

enum { WRLDWIN_EV_MOTION = 1, WRLDWIN_EV_BUTTON_DOWN, WRLDWIN_EV_BUTTON_UP, WRLDWIN_EV_WHEEL,
       WRLDWIN_EV_KEY_DOWN, WRLDWIN_EV_KEY_UP, WRLDWIN_EV_RESIZE, WRLDWIN_EV_CLOSE, WRLDWIN_EV_LEAVE };
/* button: 1 left, 2 middle, 3 right; wheel: code is the step (negative = up);
 * keys: code is THE WRLD OS key (ASCII, or 0x100 + Esc, Backspace, Tab, Enter...), ch the character;
 * resize: x, y are the new client size; close: the user clicked the close button (a second
 * click closes the window whatever the program does) */
struct wrldwin_event { uint32_t type; int32_t x, y; uint32_t code; uint32_t ch; uint32_t mods; };
#define WRLDWIN_KEY_ESC 0x100
#define WRLDWIN_KEY_BACKSPACE 0x101
#define WRLDWIN_KEY_ENTER 0x103
#endif
