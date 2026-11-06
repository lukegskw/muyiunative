// Signed product metadata: version 1 has one disc; version 2 contains an ordered set, all copied into the same app. The first disc keeps the original SaveID.
// Foundation-only so tests validate missing resources, malformed lists and path confinement without a window, game files or a GPU.
#import <Foundation/Foundation.h>
#include "product.h"
#include "ps1.h"
#include <string.h>

static BOOL copy_string(char *out, size_t cap, id value) {
    if (![value isKindOfClass:NSString.class]) return NO;
    const char *s = [value UTF8String]; if (!s || strlen(s) >= cap) return NO;
    memcpy(out, s, strlen(s) + 1); return YES;
}
static int invalid(void) { RED("host: invalid or incomplete bundled game metadata"); return -1; }
int front_product_read(front_product *p, const char *resources) { @autoreleasepool {
    NSString *base = @(resources); NSURL *url = [NSURL fileURLWithPath:[base stringByAppendingPathComponent:@"game.plist"]];
    if (![NSFileManager.defaultManager fileExistsAtPath:url.path]) return 0;
    NSDictionary *d = [NSDictionary dictionaryWithContentsOfURL:url]; NSString *save = d[@"SaveID"];
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"];
    BOOL multiple = [d[@"Version"] isEqual:@2]; NSArray *discs = d[@"Discs"];
    if ((!multiple && ![d[@"Version"] isEqual:@1]) || ![d[@"Disc"] isEqual:@"disc/disc.cue"] || ![save isKindOfClass:NSString.class] || !save.length ||
        [save rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound || !copy_string(p->save_id, sizeof p->save_id, save) ||
        !copy_string(p->title, sizeof p->title, d[@"Title"]) || (d[@"Serial"] && !copy_string(p->serial, sizeof p->serial, d[@"Serial"]))) return invalid();
    if (multiple) {
        if (![discs isKindOfClass:NSArray.class] || discs.count < 2 || discs.count > FRONT_DISC_MAX) return invalid();
    } else {
        if (discs) return invalid();
        discs = @[@{@"Title": @"Disc 1", @"Disc": @"disc/disc.cue"}];
    }
    p->ndiscs = (int)discs.count;
    for (int i = 0; i < p->ndiscs; i++) {
        id item = discs[i]; NSString *relative = i ? [NSString stringWithFormat:@"discs/disc%02d/disc.cue", i + 1] : @"disc/disc.cue";
        if (![item isKindOfClass:NSDictionary.class] || ![item[@"Disc"] isEqual:relative] || !copy_string(p->disc_titles[i], sizeof p->disc_titles[i], item[@"Title"])) return invalid();
        NSString *path = [base stringByAppendingPathComponent:relative]; NSNumber *regular;
        if (![path.stringByResolvingSymlinksInPath hasPrefix:[base.stringByResolvingSymlinksInPath stringByAppendingString:@"/"]] ||
            ![[NSURL fileURLWithPath:path] getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil] || !regular.boolValue ||
            ![NSFileManager.defaultManager isReadableFileAtPath:path] || !copy_string(p->discs[i], sizeof p->discs[i], path)) return invalid();
    }
    memcpy(p->disc, p->discs[0], sizeof p->disc);
    id renderer = d[@"Renderer"], scale = d[@"RenderScale"];
    if ((renderer && ![renderer isEqual:@"software"] && ![renderer isEqual:@"metal"]) ||
        (scale && ![scale isEqual:@1] && ![scale isEqual:@2] && ![scale isEqual:@4])) return invalid();
    p->render_scale = [renderer isEqual:@"metal"] ? (scale ? [scale intValue] : 1) : 0;
    return 1;
} }
