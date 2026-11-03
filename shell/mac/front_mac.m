// Mac front end (front.h): AppKit owns the main thread and the machine runs on a thread of its own. The window keeps the console's 4:3 picture at any size (640x480 points
// to start). Frames are handed over through one slot, so the main thread presents only the newest and a slow main thread never holds the machine back
#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/CAMetalLayer.h>
#import <GameController/GameController.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "front.h"
#include "present.h"
#include "ps1.h"
#include "keyboard.h"
#include "product.h"
#include "controllers.h"
#include "report.h"

static atomic_uint g_buttons;
static atomic_bool g_stop, g_running;
static atomic_int g_renderer;
static NSMenu *g_graphics;
static NSWindow *g_win;
static NSMenuItem *g_save_state, *g_load_state;
static atomic_int g_state_request;
static atomic_bool g_state_enabled, g_state_available, g_state_busy;
static NSString *g_title;
static BOOL g_quitting;
static os_unfair_lock g_input_lock = OS_UNFAIR_LOCK_INIT;   // only host input; neither thread shares a ps1
static front_controllers g_pads;
static GCController *g_controllers[2];
int front_stopping(void) { return atomic_load(&g_stop); }
int front_renderer(void) { return atomic_load(&g_renderer); }
static void graphics_state(void) {
    int scale = front_renderer();
    for (NSMenuItem *item in g_graphics.itemArray) item.state = item.tag == scale ? NSControlStateValueOn : NSControlStateValueOff;
}
void front_renderer_set(int scale) {
    if (scale != 0 && scale != 1 && scale != 2 && scale != 4) return;
    atomic_store(&g_renderer, scale);
    if (NSThread.isMainThread) graphics_state(); else dispatch_async(dispatch_get_main_queue(), ^{ graphics_state(); });
}
static uint16_t key_event(NSEvent *e, int down) {
    NSString *s = e.charactersIgnoringModifiers;
    return front_keyboard_event(e.keyCode, s.length ? [s characterAtIndex:0] : 0, down);
}
static void release_keys(void) { front_keyboard_reset(); atomic_store(&g_buttons, 0); }
static void state_menu_update(void) {
    BOOL enabled = atomic_load(&g_state_enabled) && !atomic_load(&g_state_busy);
    g_save_state.enabled = enabled; g_load_state.enabled = enabled && atomic_load(&g_state_available);
}
void front_state_menu(int enabled, int available) {
    atomic_store(&g_state_enabled, enabled != 0); atomic_store(&g_state_available, available != 0);
}
int front_state_request(void) { return atomic_exchange(&g_state_request, FRONT_STATE_NONE); }
void front_state_complete(int action, const char *error) { @autoreleasepool {
    NSString *message = error ? @(error) : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&g_state_busy, false);
        if (!message && action == FRONT_STATE_SAVE) atomic_store(&g_state_available, true);
        state_menu_update();
        if (message) front_error(message.UTF8String);
        else {
            g_win.title = [NSString stringWithFormat:@"%@ — State %@", g_title, action == FRONT_STATE_SAVE ? @"saved" : @"loaded"];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ g_win.title = g_title; });
        }
        if (g_quitting) atomic_store(&g_stop, true);
    });
} }
static void input_focus(int focused) {
    release_keys(); os_unfair_lock_lock(&g_input_lock);
    front_controllers_focus(&g_pads, focused); os_unfair_lock_unlock(&g_input_lock);
}
static uintptr_t controller_id(GCController *c) { return (uintptr_t)(__bridge void *)c; }
static void controllers_changed(void) {
    NSArray<GCController *> *available = GCController.controllers;
    os_unfair_lock_lock(&g_input_lock);
    for (int i = 0; i < 2; i++) if (g_controllers[i] && ![available containsObject:g_controllers[i]]) {
        front_controller_disconnect(&g_pads, controller_id(g_controllers[i])); g_controllers[i] = nil;
    }
    for (GCController *c in available) if (c.extendedGamepad) {
        int port = front_controller_connect(&g_pads, controller_id(c));
        if (port >= 0) g_controllers[port] = c;
    }
    os_unfair_lock_unlock(&g_input_lock);
}
void front_buttons(front_pad pads[2]) {
    @autoreleasepool {
        os_unfair_lock_lock(&g_input_lock);
        for (int i = 0; i < 2; i++) if (g_controllers[i]) {
            // The old front_buttons() only returned keyboard port 1. Apple's GCPhysicalInputProfile.capture freezes a profile for multithreaded polling:
            // capture here at the machine's frame boundary, rather than changing machine state from GameController callbacks on the main thread.
            GCExtendedGamepad *g = [g_controllers[i].extendedGamepad capture];
            front_controller_input in = {.south = g.buttonA.isPressed, .east = g.buttonB.isPressed, .west = g.buttonX.isPressed, .north = g.buttonY.isPressed,
                .l1 = g.leftShoulder.isPressed, .r1 = g.rightShoulder.isPressed, .menu = g.buttonMenu.isPressed, .options = g.buttonOptions.isPressed,
                .up = g.dpad.up.isPressed, .right = g.dpad.right.isPressed, .down = g.dpad.down.isPressed, .left = g.dpad.left.isPressed,
                .l2 = g.leftTrigger.value, .r2 = g.rightTrigger.value, .x = g.leftThumbstick.xAxis.value, .y = g.leftThumbstick.yAxis.value};
            front_controller_sample(&g_pads, controller_id(g_controllers[i]), in);
        }
        front_controllers_read(&g_pads, (uint16_t)atomic_load(&g_buttons), pads);
        os_unfair_lock_unlock(&g_input_lock);
    }
}

@interface FrontView : NSView
@end

@implementation FrontView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e { if (e.modifierFlags & NSEventModifierFlagCommand) [super keyDown:e]; else atomic_store(&g_buttons, key_event(e, 1)); }
- (void)keyUp:(NSEvent *)e { atomic_store(&g_buttons, key_event(e, 0)); }
@end

@interface FrontApp : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation FrontApp
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return YES; }
- (void)applicationDidResignActive:(NSNotification *)n { (void)n; input_focus(0); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; input_focus(0); }
- (void)applicationDidBecomeActive:(NSNotification *)n { (void)n; input_focus(g_win.isKeyWindow); }
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; input_focus(NSApp.isActive); }
- (void)controllersChanged:(NSNotification *)n {
    (void)n; if (NSThread.isMainThread) controllers_changed(); else dispatch_async(dispatch_get_main_queue(), ^{ controllers_changed(); });
}
- (BOOL)windowShouldClose:(NSWindow *)window { (void)window; [NSApp terminate:nil]; return NO; }
- (void)keyboard:(id)sender { (void)sender; release_keys(); front_keyboard_settings(); }
- (void)renderer:(NSMenuItem *)sender { release_keys(); front_renderer_set((int)sender.tag); }
- (void)state:(NSMenuItem *)sender {
    if (!atomic_load(&g_state_enabled) || atomic_exchange(&g_state_busy, true)) return;
    release_keys(); atomic_store(&g_state_request, (int)sender.tag); state_menu_update();
}
// AppKit's applicationShouldTerminate: contract: NSTerminateLater keeps its event loop alive while the machine thread finishes a save, then replies on the main queue.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app; if (!atomic_load(&g_running)) return NSTerminateNow;
    g_quitting = YES; if (!atomic_load(&g_state_busy)) atomic_store(&g_stop, true); return NSTerminateLater;
}
@end

void front_cancel_stop(const char *message) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        front_error(message); atomic_store(&g_stop, false);
        if (g_quitting) { g_quitting = NO; [NSApp replyToApplicationShouldTerminate:NO]; }
    });
}

typedef struct frame { uint32_t *px; size_t cap; int w, h; } frame;
typedef struct job { void (*run)(void *); void *arg; } job;

static FrontApp *g_app;   // NSApplication holds its delegate weakly
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;   // guards g_new, g_fresh and the swap
static frame g_new, g_shown;                          // the newest frame (the machine's thread writes it) and the one presented (main thread)
static bool g_fresh;
static atomic_bool g_queued;                          // a present is queued on the main thread

static int fit(frame *f, size_t n) {
    if (n <= f->cap) return 1;
    uint32_t *p = realloc(f->px, n * sizeof *p);
    if (!p) return 0;
    f->px = p; f->cap = n; return 1;
}

void front_present(const uint32_t *px, int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    os_unfair_lock_lock(&g_lock);
    if (fit(&g_new, n)) { memcpy(g_new.px, px, n * sizeof *px); g_new.w = w; g_new.h = h; g_fresh = true; }
    os_unfair_lock_unlock(&g_lock);
    if (atomic_exchange(&g_queued, true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&g_queued, false);
        os_unfair_lock_lock(&g_lock);
        bool fresh = g_fresh;
        if (fresh) { frame t = g_shown; g_shown = g_new; g_new = t; g_fresh = false; }
        os_unfair_lock_unlock(&g_lock);
        if (fresh) present_frame(g_shown.px, g_shown.w, g_shown.h);
    });
}

static void *machine_thread(void *p) {
    job j = *(job *)p; free(p);
    j.run(j.arg);
    atomic_store(&g_running, false);
    dispatch_async(dispatch_get_main_queue(), ^{ if (g_quitting) [NSApp replyToApplicationShouldTerminate:YES]; else [NSApp terminate:nil]; });
    return NULL;
}

void front_run(const char *title, void (*run)(void *arg), void *arg) {
    NSApplication *app = NSApplication.sharedApplication;
    front_keyboard_load(); atomic_store(&g_stop, false);
    g_app = [FrontApp new]; app.delegate = g_app;
    // GCController.controllers includes already paired wired/wireless devices; connection notifications also promote a waiting third device into a vacant port.
    GCController.shouldMonitorBackgroundEvents = NO;
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:g_app selector:@selector(controllersChanged:) name:GCControllerDidConnectNotification object:nil];
    [nc addObserver:g_app selector:@selector(controllersChanged:) name:GCControllerDidDisconnectNotification object:nil];
    controllers_changed();
    app.activationPolicy = NSApplicationActivationPolicyRegular;   // a Dock icon and a menu bar, also when started from a terminal
    NSString *name = @(title);
    NSMenu *bar = [NSMenu new], *menu = [NSMenu new]; NSMenuItem *item = [NSMenuItem new];
    front_reports_menu((__bridge void *)menu);
    [menu addItemWithTitle:@"Keyboard Controls…" action:@selector(keyboard:) keyEquivalent:@","].target = g_app;
    [menu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = menu; [bar addItem:item]; app.mainMenu = bar;
    NSMenu *game = [[NSMenu alloc] initWithTitle:@"Game"]; game.autoenablesItems = NO;
    g_save_state = [game addItemWithTitle:@"Save State" action:@selector(state:) keyEquivalent:@"s"];
    g_load_state = [game addItemWithTitle:@"Load State" action:@selector(state:) keyEquivalent:@"l"];
    g_save_state.tag = FRONT_STATE_SAVE; g_load_state.tag = FRONT_STATE_LOAD; g_save_state.target = g_load_state.target = g_app;
    NSMenuItem *game_item = [[NSMenuItem alloc] initWithTitle:@"Game" action:nil keyEquivalent:@""]; game_item.submenu = game; [bar addItem:game_item]; state_menu_update();
    g_graphics = [[NSMenu alloc] initWithTitle:@"Graphics"]; g_graphics.autoenablesItems = NO;
    const int scales[] = {0, 1, 2, 4}; NSArray<NSString *> *titles = @[@"Software", @"Metal (1×)", @"Metal (2×)", @"Metal (4×)"];
    for (unsigned i = 0; i < sizeof scales / sizeof *scales; i++) {
        NSMenuItem *choice = [g_graphics addItemWithTitle:titles[i] action:@selector(renderer:) keyEquivalent:@""]; choice.tag = scales[i]; choice.target = g_app;
    }
    NSMenuItem *graphics = [[NSMenuItem alloc] initWithTitle:@"Graphics" action:nil keyEquivalent:@""]; graphics.submenu = g_graphics; [bar addItem:graphics]; graphics_state();
    g_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 480)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered defer:NO];
    g_title = name; g_win.title = name; g_win.releasedWhenClosed = NO; g_win.contentAspectRatio = NSMakeSize(4, 3);
    g_win.delegate = g_app;
    FrontView *v = [[FrontView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
    v.wantsLayer = YES; v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    v.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    g_win.contentView = v;
    if (!present_init((__bridge void *)v.layer)) RED("front end: no Metal device, the window stays empty");
    [g_win center]; [g_win makeKeyAndOrderFront:nil];
    [g_win makeFirstResponder:v]; front_audio_start();
    job *j = malloc(sizeof *j);
    pthread_attr_t at; pthread_t t;
    pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8 << 20); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    atomic_store(&g_running, true);
    if (!j || (*j = (job){run, arg}, pthread_create(&t, &at, machine_thread, j))) { atomic_store(&g_running, false); free(j); RED("front end: cannot start the machine's thread"); }
    pthread_attr_destroy(&at);
    [app activate];
    [app run];
}

int front_write_png(const char *path, const uint32_t *px, int w, int h) {
    bool ok = false;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef data = CGDataProviderCreateWithData(NULL, px, (size_t)w * (size_t)h * 4, NULL);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, data, NULL, false,
                                   kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst && img) { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); }
    if (dst) CFRelease(dst);
    if (url) CFRelease(url);
    CGImageRelease(img); CGDataProviderRelease(data); CGColorSpaceRelease(cs);
    if (!ok) RED("front end: cannot write a PNG to %s", path);
    return ok ? 0 : -1;
}
