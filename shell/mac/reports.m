// Phase 7's app diagnostics: Foundation supplies bundle/version and OS information; AppKit reveals the sanitized log and opens a prefilled GitHub issue form.
// A player reviews and submits the form themselves. The host retains no disc bytes, paths or screenshots in the URL; only the bundled game's serial is used.
#import <Cocoa/Cocoa.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <stdatomic.h>
#include "front.h"
#include "product.h"
#include "log.h"
#include "report.h"
#include "version.h"

static front_log g_log;
static NSString *g_version, *g_commit, *g_device, *g_game;
static int g_playing;
static atomic_int g_guest_output = 1;
int front_reports_guest_output(void) { return atomic_load(&g_guest_output); }
void front_reports_tty(void *ctx, const char *line) { (void)ctx; if (front_reports_guest_output()) fprintf(stderr, "tty: %s\n", line); }
static NSString *bundle_string(NSString *key, const char *fallback) {
    id value = [NSBundle.mainBundle objectForInfoDictionaryKey:key]; return [value isKindOfClass:NSString.class] && [value length] ? value : @(fallback);
}
static void stop_log(void) { front_log_stop(&g_log); }
void front_reports_start(int headless) {
    @autoreleasepool {
        NSString *version = bundle_string(@"CFBundleShortVersionString", FRONT_VERSION), *build = bundle_string(@"CFBundleVersion", FRONT_BUILD);
        g_commit = bundle_string(@"MuyiunativeCommit", FRONT_COMMIT); g_version = [NSString stringWithFormat:@"%@ (build %@)", version, build];
        char model[256] = "Unknown Mac"; size_t n = sizeof model; sysctlbyname("hw.model", model, &n, NULL, 0); model[sizeof model - 1] = 0;
        NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        g_device = [NSString stringWithFormat:@"%s, macOS %ld.%ld.%ld", model, (long)os.majorVersion, (long)os.minorVersion, (long)os.patchVersion]; g_game = @"";
        NSString *header = [NSString stringWithFormat:@"host: muyiunative %@, commit %@\nhost: %@\n", g_version, g_commit, g_device];
        // Headless stdout is a machine-readable build/lockstep result. Terminal sessions keep their streams; Finder's otherwise discarded output gets a file.
        if (!headless && !isatty(STDOUT_FILENO) && !isatty(STDERR_FILENO)) {
            atomic_store(&g_guest_output, 0);   // user-approved Finder default; the session menu can enable guest text without changing the machine
            NSString *identifier = NSBundle.mainBundle.bundleIdentifier ?: @"org.muyiunative.developer";
            NSCharacterSet *safe = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"];
            NSURL *library = [NSFileManager.defaultManager URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask].firstObject;
            NSURL *folder = [[library URLByAppendingPathComponent:@"Logs/muyiunative" isDirectory:YES] URLByAppendingPathComponent:identifier isDirectory:YES];
            NSError *error;
            if ([identifier rangeOfCharacterFromSet:safe.invertedSet].location != NSNotFound || [identifier isEqual:@"."] || [identifier isEqual:@".."] ||
                !folder || ![NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:&error] ||
                front_log_start(&g_log, folder.path.fileSystemRepresentation, NSHomeDirectory().fileSystemRepresentation, header.UTF8String))
                fprintf(stderr, "[RED] host: cannot create a run log\n");
            else { atexit(stop_log); fprintf(stderr, "host: log %s\nhost: game debug output disabled; run log limited to 10 MiB\n", g_log.path); return; }
        }
        fputs(header.UTF8String, stderr);
    }
}
void front_reports_game(const char *serial) { g_playing = 1; g_game = serial ? @(serial) : @""; }

@interface ReportMenu : NSObject
- (void)about:(id)sender;
- (void)report:(id)sender;
- (void)guest:(NSMenuItem *)sender;
@end
@implementation ReportMenu
- (void)guest:(NSMenuItem *)sender {
    int enabled = !front_reports_guest_output(); atomic_store(&g_guest_output, enabled); sender.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
    fprintf(stderr, "host: game debug output %s for this run\n", enabled ? "enabled" : "disabled");
}
- (void)about:(id)sender {
    (void)sender; NSAlert *a = [NSAlert new]; a.messageText = @"About muyiunative";
    a.informativeText = [NSString stringWithFormat:@"muyiunative %@\nCommit %@\n%@", g_version, g_commit, g_device]; [a runModal];
}
- (void)report:(id)sender {
    (void)sender; fflush(NULL);
    int scale = front_renderer(); NSString *renderer = !g_playing ? @"Launcher (no renderer)" : scale ? [NSString stringWithFormat:@"Metal (%d×)", scale] : @"Software";
    front_report_info info = {g_version.UTF8String, g_commit.UTF8String, g_device.UTF8String, g_game.UTF8String, renderer.UTF8String}; char url[8192];
    if (front_report_url(url, sizeof url, &info)) { front_error("The problem-report URL is too long."); return; }
    if (g_log.path[0]) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:@(g_log.path)]]];
    else front_error("This run writes to the terminal. Copy its output for the report, or launch the app from Finder to save a log file.");
    if (![NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@(url)]]) front_error("Cannot open the problem-report form in your browser.");
}
@end

void front_reports_menu(void *menu) {
    static ReportMenu *actions; if (!actions) actions = [ReportMenu new]; NSMenu *m = (__bridge NSMenu *)menu;
    [m addItemWithTitle:@"About muyiunative…" action:@selector(about:) keyEquivalent:@""].target = actions;
    [m addItemWithTitle:@"Report a Problem…" action:@selector(report:) keyEquivalent:@""].target = actions;
    NSMenuItem *guest = [m addItemWithTitle:@"Include Game Debug Output" action:@selector(guest:) keyEquivalent:@""]; guest.target = actions;
    guest.state = front_reports_guest_output() ? NSControlStateValueOn : NSControlStateValueOff;
    [m addItem:NSMenuItem.separatorItem];
}
