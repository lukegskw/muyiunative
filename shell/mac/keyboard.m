// Keyboard remapping for the Mac product (plan.md, phase 5): logical characters, physical-key releases, and shared preferences across per-game bundles.
// AppKit's charactersIgnoringModifiers supplies the key; NSEvent keyCode identifies its release. No console state or game data belongs in preferences.
#import <Cocoa/Cocoa.h>
#include "keyboard.h"
#include "keys.h"

static front_keys g_keys;
static NSUserDefaults *g_prefs;
static const unsigned buttons[] = {4, 7, 6, 5, 12, 15, 14, 13, 10, 11, 8, 9, 3, 0};
static NSString *const names[] = {@"Up", @"Left", @"Down", @"Right", @"Triangle", @"Square", @"Cross", @"Circle", @"L1", @"R1", @"L2", @"R2", @"Start", @"Select"};

static NSString *key_name(uint32_t key) {
    switch (key) {
    case 0: return @"Unassigned"; case '\r': return @"Return / Enter"; case 0x7f: return @"Backspace (⌫)";
    case '\t': return @"Tab"; case ' ': return @"Space";
    case NSUpArrowFunctionKey: return @"↑"; case NSDownArrowFunctionKey: return @"↓";
    case NSLeftArrowFunctionKey: return @"←"; case NSRightArrowFunctionKey: return @"→";
    case NSDeleteFunctionKey: return @"Forward Delete"; case NSHomeFunctionKey: return @"Home"; case NSEndFunctionKey: return @"End";
    case NSPageUpFunctionKey: return @"Page Up"; case NSPageDownFunctionKey: return @"Page Down";
    default:
        if (key >= NSF1FunctionKey && key <= NSF35FunctionKey) return [NSString stringWithFormat:@"F%u", key - NSF1FunctionKey + 1];
        return [[NSString stringWithFormat:@"%C", (unichar)key] uppercaseString];
    }
}

static void save_keys(void) {
    NSMutableArray *keys = [NSMutableArray new];
    for (unsigned i = 0; i < FRONT_KEY_COUNT; i++) [keys addObject:@(g_keys.key[i])];
    [g_prefs setObject:keys forKey:@"keyboard-v1"];
}

void front_keyboard_load(void) {
    front_keys_defaults(&g_keys); g_prefs = [[NSUserDefaults alloc] initWithSuiteName:@"org.ps1native"];
    id saved = [g_prefs objectForKey:@"keyboard-v1"];
    if (![saved isKindOfClass:NSArray.class] || [saved count] != FRONT_KEY_COUNT) return;
    uint32_t values[FRONT_KEY_COUNT];
    for (unsigned i = 0; i < FRONT_KEY_COUNT; i++) {
        id value = saved[i];
        if (![value isKindOfClass:NSNumber.class] || [value unsignedLongLongValue] > 0xffff) return;
        values[i] = [value unsignedIntValue];
    }
    front_keys_load(&g_keys, values, FRONT_KEY_COUNT);
}

uint16_t front_keyboard_event(uint16_t keycode, uint32_t character, int down) { return front_keys_event(&g_keys, keycode, character, down); }
void front_keyboard_reset(void) { front_keys_reset(&g_keys); }

@interface KeyboardSettings : NSWindowController
@property(strong) NSMutableArray<NSButton *> *bindings;
@property(strong) NSTextField *hint;
@property(strong) NSButton *clear;
@property NSInteger selected;
@property(strong) id monitor;
- (void)refresh;
@end

@implementation KeyboardSettings
- (instancetype)init {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 550)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:window])) return nil;
    window.title = @"Keyboard Controls"; window.releasedWhenClosed = NO; self.selected = -1; self.bindings = [NSMutableArray new];
    self.hint = [NSTextField wrappingLabelWithString:@"Click a binding, then press a key. Changes apply to all games."];
    self.hint.frame = NSMakeRect(20, 500, 400, 35); [window.contentView addSubview:self.hint];
    for (unsigned i = 0; i < sizeof buttons / sizeof *buttons; i++) {
        NSTextField *label = [NSTextField labelWithString:names[i]]; label.frame = NSMakeRect(25, 473 - i * 29, 115, 22); [window.contentView addSubview:label];
        NSButton *binding = [NSButton buttonWithTitle:@"" target:self action:@selector(selectBinding:)];
        binding.frame = NSMakeRect(145, 470 - i * 29, 270, 26); binding.tag = i;
        [self.bindings addObject:binding]; [window.contentView addSubview:binding];
    }
    NSButton *defaults = [NSButton buttonWithTitle:@"Restore Defaults" target:self action:@selector(defaults:)]; defaults.frame = NSMakeRect(20, 25, 140, 32);
    [window.contentView addSubview:defaults];
    self.clear = [NSButton buttonWithTitle:@"Unassign" target:self action:@selector(clearBinding:)]; self.clear.frame = NSMakeRect(175, 25, 110, 32);
    [window.contentView addSubview:self.clear];
    NSButton *done = [NSButton buttonWithTitle:@"Done" target:window action:@selector(close)]; done.frame = NSMakeRect(325, 25, 90, 32);
    [window.contentView addSubview:done];
    __weak KeyboardSettings *weak = self;
    self.monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
        KeyboardSettings *owner = weak;
        if (!owner || event.window != owner.window || owner.selected < 0 || (event.modifierFlags & NSEventModifierFlagCommand)) return event;
        if (event.keyCode == 53) { owner.selected = -1; [owner refresh]; return nil; }
        NSString *s = event.charactersIgnoringModifiers;
        if (s.length != 1 || (event.modifierFlags & NSEventModifierFlagControl) ||
            front_keys_assign(&g_keys, buttons[owner.selected], [s characterAtIndex:0])) { NSBeep(); return nil; }
        save_keys(); owner.selected = -1; [owner refresh]; return nil;
    }];
    [self refresh]; [window center]; return self;
}
- (void)refresh {
    for (unsigned i = 0; i < self.bindings.count; i++) self.bindings[i].title = self.selected == (NSInteger)i ? @"Press a key…" : key_name(g_keys.key[buttons[i]]);
    self.clear.enabled = self.selected >= 0;
    self.hint.stringValue = self.selected >= 0 ? [NSString stringWithFormat:@"Press a key for %@. Escape cancels; Unassign clears it. A key can control one button.", names[self.selected]] :
        @"Click a binding, then press a key. Changes apply to all games.";
}
- (void)selectBinding:(NSButton *)sender { self.selected = sender.tag; front_keyboard_reset(); [self refresh]; }
- (void)clearBinding:(id)sender { (void)sender; if (self.selected >= 0) { front_keys_assign(&g_keys, buttons[self.selected], 0); save_keys(); self.selected = -1; [self refresh]; } }
- (void)defaults:(id)sender { (void)sender; front_keys_defaults(&g_keys); save_keys(); self.selected = -1; [self refresh]; }
@end

void front_keyboard_settings(void) {
    static KeyboardSettings *settings; if (!settings) settings = [KeyboardSettings new];
    front_keyboard_reset(); settings.selected = -1; [settings refresh]; [settings showWindow:nil]; [settings.window makeKeyAndOrderFront:nil];
}
