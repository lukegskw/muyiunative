// Mac front end (front.h): AppKit owns the main thread and the machine runs on a thread of its own. The window keeps the console's 4:3 picture at any size (640x480 points
// to start). Frames are handed over through one slot, so the main thread presents only the newest and a slow main thread never holds the machine back
#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AudioToolbox/AudioToolbox.h>
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

static atomic_uint g_buttons;
static atomic_bool g_stop, g_running;
static BOOL g_quitting;
int front_stopping(void) { return atomic_load(&g_stop); }
static uint16_t key_event(NSEvent *e, int down) {
    NSString *s = e.charactersIgnoringModifiers;
    return front_keyboard_event(e.keyCode, s.length ? [s characterAtIndex:0] : 0, down);
}
static void release_keys(void) { front_keyboard_reset(); atomic_store(&g_buttons, 0); }
uint16_t front_buttons(void) { return (uint16_t)atomic_load(&g_buttons); }

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
- (void)applicationDidResignActive:(NSNotification *)n { (void)n; release_keys(); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; release_keys(); }
- (BOOL)windowShouldClose:(NSWindow *)window { (void)window; [NSApp terminate:nil]; return NO; }
- (void)keyboard:(id)sender { (void)sender; release_keys(); front_keyboard_settings(); }
// AppKit's applicationShouldTerminate: contract: NSTerminateLater keeps its event loop alive while the machine thread finishes a save, then replies on the main queue.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app; if (!atomic_load(&g_running)) return NSTerminateNow;
    g_quitting = YES; atomic_store(&g_stop, true); return NSTerminateLater;
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
static NSWindow *g_win;
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;   // guards g_new, g_fresh and the swap
static frame g_new, g_shown;                          // the newest frame (the machine's thread writes it) and the one presented (main thread)
static bool g_fresh;
static atomic_bool g_queued;                          // a present is queued on the main thread

// Audio Queue Services owns its playback thread; only the front end's bounded ring is shared. The runtime's audio ring is drained on its machine thread
enum { AUDIO_CAP = 8192, AUDIO_CHUNK = 512 };
static os_unfair_lock g_audio_lock = OS_UNFAIR_LOCK_INIT;
static int16_t g_audio[AUDIO_CAP * 2]; static unsigned g_audio_read, g_audio_count;
static AudioQueueRef g_audio_queue;
static atomic_bool g_audio_failed;
void front_audio(const int16_t *samples, int frames) {
    os_unfair_lock_lock(&g_audio_lock);
    for (int i = 0; i < frames; i++) {
        if (g_audio_count == AUDIO_CAP) { g_audio_read = (g_audio_read + 1) % AUDIO_CAP; g_audio_count--; }
        unsigned at = (g_audio_read + g_audio_count++) % AUDIO_CAP;
        g_audio[at * 2] = samples[i * 2]; g_audio[at * 2 + 1] = samples[i * 2 + 1];
    }
    os_unfair_lock_unlock(&g_audio_lock);
}
static OSStatus audio_fill(AudioQueueRef q, AudioQueueBufferRef b) {
    int16_t *out = b->mAudioData;
    memset(out, 0, AUDIO_CHUNK * 4);
    os_unfair_lock_lock(&g_audio_lock);
    for (unsigned i = 0; i < AUDIO_CHUNK && g_audio_count; i++, g_audio_count--) {
        out[i * 2] = g_audio[g_audio_read * 2]; out[i * 2 + 1] = g_audio[g_audio_read * 2 + 1]; g_audio_read = (g_audio_read + 1) % AUDIO_CAP;
    }
    os_unfair_lock_unlock(&g_audio_lock);
    b->mAudioDataByteSize = AUDIO_CHUNK * 4;
    return AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
static void audio_buffer(void *arg, AudioQueueRef q, AudioQueueBufferRef b) {
    (void)arg; OSStatus err = audio_fill(q, b);
    if (err && !atomic_exchange(&g_audio_failed, true)) RED("front end: cannot enqueue audio (%d)", (int)err);
}
static void audio_start(void) {
    AudioStreamBasicDescription format = {.mSampleRate = 44100, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, .mBytesPerPacket = 4, .mFramesPerPacket = 1,
        .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    OSStatus err = AudioQueueNewOutput(&format, audio_buffer, NULL, NULL, NULL, 0, &g_audio_queue);
    for (int i = 0; !err && i < 3; i++) {
        AudioQueueBufferRef b; err = AudioQueueAllocateBuffer(g_audio_queue, AUDIO_CHUNK * 4, &b);
        if (!err) err = audio_fill(g_audio_queue, b);
    }
    if (!err) err = AudioQueueStart(g_audio_queue, NULL);
    if (err) { RED("front end: cannot start audio (%d)", (int)err); if (g_audio_queue) AudioQueueDispose(g_audio_queue, true); g_audio_queue = NULL; }
}

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
    app.activationPolicy = NSApplicationActivationPolicyRegular;   // a Dock icon and a menu bar, also when started from a terminal
    NSString *name = @(title);
    NSMenu *bar = [NSMenu new], *menu = [NSMenu new]; NSMenuItem *item = [NSMenuItem new];
    [menu addItemWithTitle:@"Keyboard Controls…" action:@selector(keyboard:) keyEquivalent:@","].target = g_app;
    [menu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = menu; [bar addItem:item]; app.mainMenu = bar;
    g_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 480)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered defer:NO];
    g_win.title = name; g_win.releasedWhenClosed = NO; g_win.contentAspectRatio = NSMakeSize(4, 3);
    g_win.delegate = g_app;
    FrontView *v = [[FrontView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
    v.wantsLayer = YES; v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    v.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    g_win.contentView = v;
    if (!present_init((__bridge void *)v.layer)) RED("front end: no Metal device, the window stays empty");
    [g_win center]; [g_win makeKeyAndOrderFront:nil];
    [g_win makeFirstResponder:v]; audio_start();
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
