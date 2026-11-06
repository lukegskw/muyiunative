// Ordered bundled discs and legacy metadata, read without an app/window/GPU. Every resource is synthetic and lives in a temporary directory.
#import <Foundation/Foundation.h>
#include "../product.h"
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
int main(void) { @autoreleasepool {
    char temp[] = "/tmp/muyiunative-catalog-XXXXXX"; if (!mkdtemp(temp)) return 1;
    NSString *base = @(temp), *metadata = [base stringByAppendingPathComponent:@"game.plist"];
    front_product product = {0}; NSFileManager *files = NSFileManager.defaultManager;
    CHECK(front_product_read(&product, temp) == 0, "a launcher without game metadata has no disc catalog");
    NSMutableArray *discs = [NSMutableArray new];
    for (int i = 0; i < 4; i++) {
        NSString *relative = i ? [NSString stringWithFormat:@"discs/disc%02d/disc.cue", i + 1] : @"disc/disc.cue";
        NSString *path = [base stringByAppendingPathComponent:relative];
        [files createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        [@"synthetic" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [discs addObject:@{@"Title": [NSString stringWithFormat:@"Disc %d", i + 1], @"Disc": relative}];
    }
    NSMutableDictionary *game = [@{@"Version": @2, @"Title": @"Synthetic Set", @"SaveID": @"first-disc-identity", @"Disc": @"disc/disc.cue", @"Discs": discs} mutableCopy];
    [game writeToFile:metadata atomically:YES];
    CHECK(front_product_read(&product, temp) == 1 && product.ndiscs == 4 && !strcmp(product.save_id, "first-disc-identity"), "four discs share the original card identity");
    CHECK(strstr(product.discs[3], "/discs/disc04/disc.cue") && !strcmp(product.disc_titles[3], "Disc 4") && !strcmp(product.disc, product.discs[0]), "order, labels and first-disc compatibility survive loading");
    for (id invalid in @[@[], @[@"wrong item"], @[@{}, @{}], @[@{@"Title": @"First", @"Disc": @"../outside.cue"}, discs[1]], @"wrong type"]) {
        game[@"Discs"] = invalid; [game writeToFile:metadata atomically:YES];
        CHECK(front_product_read(&product, temp) == -1, "malformed or escaping catalogs are rejected");
    }
    game[@"Discs"] = discs; [game writeToFile:metadata atomically:YES];
    NSString *fourth = [base stringByAppendingPathComponent:discs[3][@"Disc"]]; [files removeItemAtPath:fourth error:nil];
    CHECK(front_product_read(&product, temp) == -1, "a missing later disc prevents launching an incomplete set");
    [files createSymbolicLinkAtPath:fourth withDestinationPath:@"/dev/null" error:nil];
    CHECK(front_product_read(&product, temp) == -1, "a symlink cannot move a bundled disc outside Resources");
    game[@"Version"] = @1; [game removeObjectForKey:@"Discs"]; [game writeToFile:metadata atomically:YES];
    CHECK(front_product_read(&product, temp) == 1 && product.ndiscs == 1, "existing single-disc apps retain their metadata and card identity");
    [files removeItemAtPath:base error:nil]; printf("catalog: %d checks, %d failed\n", checks, fails); return fails != 0;
} }
