import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zlib


class SvenBuildScriptsTest(unittest.TestCase):
    def test_ios_touch_viewport_and_editor_preserve_coordinates(self):
        repository = Path(__file__).resolve().parents[3]
        source = (repository / "engine/client/input/in_touch.c").read_text()

        def function(signature):
            start = source.index(signature)
            opening = source.index("{", start)
            depth = 1
            end = opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]

        harness = r'''
#include <assert.h>
#include <math.h>
#include <stddef.h>
#define Q_max(first, second) ((first) > (second) ? (first) : (second))
enum { state_none, state_edit, state_edit_move, true = 1 };
typedef struct touch_button_s {
    float x1, y1, x2, y2;
    struct touch_button_s *next;
} touch_button_t;
static struct { float value; } touch_safearea = {1};
static struct { float window_insets[4]; } host = {{0.05f, 0.02f, 0.05f, 0.06f}};
static struct { int width, height; float scale_x, scale_y; } refState = {1000, 750, 1, 1};
static struct {
    float view_x, view_y, view_width, view_height;
    float actual_aspect_ratio, config_aspect_ratio;
    int state, resize_finger, move_finger, look_finger, wheel_finger, configchanged;
    void *move_button;
    struct { touch_button_t *first; } list_user;
} touch;
static int stretch;
static int Touch_StretchPixels(void) { return stretch; }
static void Touch_ResetSticks(void) {}
'''
        harness += function("static void Touch_UpdateViewport( void )") + "\n"
        harness += function("static void Touch_EnableEdit_f( void )") + "\n"
        harness += r'''
int main(void) {
    Touch_UpdateViewport();
#if XASH_IOS
    assert(touch.view_x == 0 && touch.view_y == 0);
    assert(touch.view_width == 1000 && touch.view_height == 750);
#else
    assert(touch.view_x == 50 && touch.view_y == 15);
    assert(fabsf(touch.view_width - 900) < 0.001f);
    assert(fabsf(touch.view_height - 690) < 0.001f);
#endif
    stretch = 1;
    Touch_UpdateViewport();
    assert(touch.view_x == 0 && touch.view_y == 0);
    assert(touch.view_width == 1000 && touch.view_height == 750);
    touch_button_t button = {0.6f, 0.6f, 0.8f, 0.9f, NULL};
    touch.list_user.first = &button;
    touch.actual_aspect_ratio = touch.config_aspect_ratio = 0.5f;
    Touch_EnableEdit_f();
    assert(touch.state == state_edit);
    assert(touch.resize_finger == -1 && touch.move_finger == -1);
    assert(touch.look_finger == -1 && touch.wheel_finger == -1);
    assert(button.x1 == 0.6f && button.x2 == 0.8f);
#if XASH_IOS
    assert(button.y1 == 0.6f && button.y2 == 0.9f);
    for (int repeat = 0; repeat < 4; ++repeat) {
        refState.width = repeat % 2 ? 1000 : 1500;
        touch.state = state_none;
        Touch_EnableEdit_f();
        Touch_UpdateViewport();
        assert(button.y1 == 0.6f && button.y2 == 0.9f);
        assert(touch.view_width == refState.width && touch.view_height == refState.height);
    }
#else
    assert(fabsf(button.y1 - 0.4f) < 0.001f);
    assert(fabsf(button.y2 - 0.6f) < 0.001f);
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "touch-viewport.c"
            test_source.write_text(harness)
            for ios in (0, 1):
                executable = Path(temporary) / ("touch-viewport-" + str(ios))
                subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-DXASH_IOS=" + str(ios),
                                str(test_source), "-lm", "-o", str(executable)], check=True)
                subprocess.run([str(executable)], check=True)

    def test_captured_sven_delta_descriptions_and_clientdata(self):
        repository = Path(__file__).resolve().parents[3]
        fixture = json.loads((Path(__file__).parent / "fixtures/sven_delta_packets.json").read_text())
        includes = ["engine", "engine/common", "engine/client", "engine/client/vgui", "engine/server",
                    "engine/platform", "public", "common", "filesystem", "pm_shared", "3rdparty/library_suffix/include"]
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "sven-delta-test"
            descriptions = Path(temporary) / "descriptions.bin"
            descriptions.write_bytes(zlib.decompress(base64.b64decode(fixture["descriptions_zlib_base64"])))
            dead_strip = "-Wl,-dead_strip" if os.uname().sysname == "Darwin" else "-Wl,--gc-sections"
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-DENGINE_DLL", "-DXASH_DEDICATED=1", "-DHAVE_STRCASECMP",
                       "-DHAVE_STRNCASECMP", "-ffunction-sections", "-fdata-sections", "-Wno-pointer-sign",
                       "-Wno-initializer-overrides"]
            command += ["-I" + str(repository / include) for include in includes]
            command += [str(Path(__file__).parent / "SvenDeltaTests.c"),
                        str(repository / "engine/common/net_buffer.c"), str(repository / "public/crtlib.c"),
                        str(repository / "public/xash3d_mathlib.c"), dead_strip, "-lm", "-o", str(executable)]
            subprocess.run(command, check=True)
            for capture in fixture["cases"]:
                packet = Path(temporary) / (capture["name"] + ".bin")
                packet.write_bytes(bytes.fromhex(capture["hex"]))
                subprocess.run([str(executable), str(descriptions), str(packet),
                                str(capture["health"]), str(capture["bits"])], check=True)

    def test_installed_directory_and_visible_immersive_option(self):
        repository = Path(__file__).resolve().parents[3]
        downloader = (repository / "ios/Xash3D-iOS/model/GameDataDownloader.mm").read_text()
        installed = downloader.split("- (BOOL)hasGameData:", 1)[1].split("- (void)downloadGame:", 1)[0]
        self.assertIn("fileExistsAtPath:path isDirectory:&isDir] && isDir", installed)
        self.assertNotIn("contentsOfDirectoryAtPath", installed)
        steam = (repository / "ios/Xash3D-iOS/view/SteamViewController.inc").read_text()
        self.assertIn('button:installTitle action:@selector(installTapped)', steam)
        self.assertIn("UIApplicationDidBecomeActiveNotification", steam)
        browser = (repository / "ios/Xash3D-iOS/view/FileBrowserViewController.mm").read_text()
        launch = browser.split("- (void)launchTapped", 1)[1].split("- (void)steamTapped", 1)[0]
        self.assertIn("launch.modalPresentationStyle = UIModalPresentationPageSheet", launch)
        self.assertIn("UISheetPresentationControllerDetent.largeDetent", launch)
        self.assertNotIn("mediumDetent", launch)
        options = (repository / "ios/Xash3D-iOS/view/LaunchOptionsViewController.inc").read_text()
        self.assertLess(options.index("addArrangedSubview:_argumentsField"), options.index("addArrangedSubview:immersiveRow"))
        self.assertIn("immersiveRow.heightAnchor constraintGreaterThanOrEqualToConstant:44", options)
        self.assertIn("CGSizeMake(420, 360)", options)
        self.assertIn("_immersiveSwitch.accessibilityLabel", options)
        self.assertIn("stack.widthAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.widthAnchor constant:-48", options)

    def test_fragment_batches_are_drained_before_the_next_packet(self):
        repository = Path(__file__).resolve().parents[3]
        client = (repository / "engine/client/cl_main.c").read_text()
        network = (repository / "engine/common/net_chan.c").read_text()

        def function(source, signature):
            start = source.index(signature)
            opening = source.index("{", start)
            depth, end = 1, opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]

        reader = function(client, "static void CL_ReadNetMessage( void )")
        rejected = reader.index("if( !Netchan_Process(")
        self.assertIn("CL_ProcessIncomingFragments( parsefn );", reader[rejected:reader.index("continue;", rejected)])
        self.assertIn("CL_ParseNetMessage( &net_message, parsefn );\n\t\tCL_ProcessIncomingFragments( parsefn );", reader)
        program = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define FRAG_GETID(value) (((value) >> 16) & 65535)
#define FRAG_GETCOUNT(value) ((value) & 65535)
#define FRAG_NORMAL_STREAM 0
#define ca_connected 3
typedef bool qboolean;
typedef struct fragbuf_s { int bufferid; struct fragbuf_s *next; char value; } fragbuf_t;
typedef struct { bool incomingready[2]; fragbuf_t *incomingbufs[2]; char incomingfilename[32]; } netchan_t;
typedef struct { char *data; size_t size; } sizebuf_t;
struct { int state; netchan_t netchan; } cls;
char net_message_buffer[128];
sizebuf_t net_message;
static unsigned batches, fileCopies;
bool Netchan_IncomingReady(netchan_t *channel) { return channel->incomingready[0] || channel->incomingready[1]; }
bool Netchan_CopyNormalFragments(netchan_t *channel,sizebuf_t *message,size_t *size) {
 (void)message;
 if(!channel->incomingready[0]) return false;
 *size=0;
 for(fragbuf_t *fragment=channel->incomingbufs[0];fragment;fragment=fragment->next)
  net_message_buffer[(*size)++]=fragment->value;
 channel->incomingbufs[0]=NULL; channel->incomingready[0]=false;
 return true;
}
void MSG_Init(sizebuf_t *message,const char *name,char *data,size_t size) {
 (void)name; message->data=data; message->size=size;
}
void CL_ParseNetMessage(sizebuf_t *message,void (*parse)(sizebuf_t *)) { parse(message); }
bool Netchan_CopyFileFragments(netchan_t *channel,sizebuf_t *message) {
 (void)channel; (void)message; ++fileCopies; return false;
}
void CL_ProcessFile(bool success,const char *name) { (void)success; (void)name; assert(false); }
"""
        program += function(network, "void Netchan_AddBufferToList( fragbuf_t **pplist, fragbuf_t *pbuf )\n{")
        program += function(network, "static void Netchan_CheckForCompletion(")
        program += function(client, "static void CL_ProcessIncomingFragments(")
        program += r"""
void parse(sizebuf_t *message) {
 if(batches==0) assert(message->size==1 && message->data[0]=='P');
 else assert(message->size==4 && !memcmp(message->data,"BZ2!",4));
 ++batches;
}
int main(void) {
 cls.state=ca_connected;
 fragbuf_t prefix={(1<<16)|1,NULL,'P'};
 Netchan_AddBufferToList(&cls.netchan.incomingbufs[0],&prefix);
 Netchan_CheckForCompletion(&cls.netchan,0,1);
 CL_ProcessIncomingFragments(parse);
 assert(batches==1 && !cls.netchan.incomingbufs[0]);
 fragbuf_t fragments[4]={ {(1<<16)|4,NULL,'B'}, {(2<<16)|4,NULL,'Z'},
                         {(3<<16)|4,NULL,'2'}, {(4<<16)|4,NULL,'!'} };
 const int order[]={1,3,0,2};
 for(unsigned index=0;index<4;++index) {
  Netchan_AddBufferToList(&cls.netchan.incomingbufs[0],&fragments[order[index]]);
  Netchan_CheckForCompletion(&cls.netchan,0,4);
  assert(cls.netchan.incomingready[0]==(index==3));
  CL_ProcessIncomingFragments(parse);
  assert(batches==(index==3?2:1));
 }
 fragbuf_t mismatched[2]={ {(1<<16)|2,NULL,'a'}, {(2<<16)|3,NULL,'b'} };
 Netchan_AddBufferToList(&cls.netchan.incomingbufs[0],&mismatched[0]);
 Netchan_AddBufferToList(&cls.netchan.incomingbufs[0],&mismatched[1]);
 Netchan_CheckForCompletion(&cls.netchan,0,2);
 assert(!cls.netchan.incomingready[0]);
 cls.state=0; cls.netchan.incomingready[1]=true;
 unsigned before=fileCopies; CL_ProcessIncomingFragments(parse); assert(fileCopies==before);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "fragments.c"
            executable = Path(temporary) / "fragments-test"
            source.write_text(program)
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_downloaded_model_overrides_stale_missing_flag(self):
        repository = Path(__file__).resolve().parents[3]
        source = (repository / "engine/client/cl_main.c").read_text()
        start = source.index("static model_t *CL_PrecacheResourceModel(")
        end = source.index("qboolean CL_PrecacheResources(", start)
        program = """
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#define S_ERROR ""
#define RES_WASMISSING 1
#define FBitSet(value, bits) ((value) & (bits))
#define ClearBits(value, bits) ((value) &= ~(bits))
#define Q_stricmp strcasecmp
typedef struct { int marker; } model_t;
typedef struct { unsigned ucFlags; char szFileName[128]; } resource_t;
static model_t original, fallback;
static bool available;
static bool customSpriteFallback = true;
static const char *loaded;
const char *COM_FileExtension(const char *path) { const char *dot = strrchr(path, '.'); return dot ? dot + 1 : ""; }
bool FS_FileExists(const char *path, bool gameOnly) { (void)gameOnly; return !strcmp(path, "models/error.spr") ? customSpriteFallback : available; }
void Con_Printf(const char *format, ...) { (void)format; }
model_t *Mod_ForName(const char *path, bool crash, bool crc) {
    (void)crash; (void)crc; loaded = path;
    if (!strncmp(path, "models/error.", 13) || !strcmp(path, "sprites/error.spr")) return &fallback;
    return available ? &original : 0;
}
""" + source[start:end] + """
int main(void) {
    resource_t resource = { RES_WASMISSING, "models/Evilmansion/furniture/arc_fern.mdl" };
    available = true;
    assert(CL_PrecacheResourceModel(&resource) == &original);
    assert(!(resource.ucFlags & RES_WASMISSING));
    assert(!strcmp(loaded, resource.szFileName));
    available = false; resource.ucFlags = RES_WASMISSING;
    assert(CL_PrecacheResourceModel(&resource) == &fallback);
    assert(!strcmp(loaded, "models/error.mdl"));
    strcpy(resource.szFileName, "sprites/dynamic_mapvote/theyhunger.SPR");
    assert(CL_PrecacheResourceModel(&resource) == &fallback);
    assert(!strcmp(loaded, "models/error.spr"));
    customSpriteFallback = false;
    assert(CL_PrecacheResourceModel(&resource) == &fallback);
    assert(!strcmp(loaded, "sprites/error.spr"));
    resource.ucFlags = 0;
    assert(CL_PrecacheResourceModel(&resource) == 0);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "model-fallback-test.c"
            executable = Path(temporary) / "model-fallback-test"
            test_source.write_text(program)
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test_source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_content_file_retries_are_bounded_and_cancellable(self):
        repository = Path(__file__).resolve().parents[3]
        source = (repository / "ios/Xash3D-iOS/model/GameDataDownloader.mm").read_text()
        start = source.index("static BOOL assembleFileWithRetry(")
        end = source.index("#pragma mark - GameDataDownloader", start)
        retry = source[start:end].replace("BOOL (^cancelled)(void)", "BOOL (*cancelled)(void)").replace('@"', '"')
        program = """
#include <cassert>
#define YES true
#define NO false
using BOOL = bool;
struct NSDictionary {};
struct NSString {};
struct NSData {};
struct NSArray {};
static int attempts, succeedAfter, delays;
static bool cancelled, cancelDuringDelay;
void logToFile(const char *, ...) {}
int usleep(unsigned) { delays++; if (cancelDuringDelay) cancelled = true; return 0; }
bool isCancelled() { return cancelled; }
bool assembleFile(int, NSDictionary *, NSString *, NSData *, NSArray *, bool (*)(void)) {
    attempts++; return attempts >= succeedAfter;
}
""" + retry + """
int main() {
    succeedAfter = 2;
    assert(assembleFileWithRetry(1, nullptr, nullptr, nullptr, nullptr, isCancelled));
    assert(attempts == 2 && delays > 0);
    attempts = delays = 0; succeedAfter = 99;
    assert(!assembleFileWithRetry(1, nullptr, nullptr, nullptr, nullptr, isCancelled));
    assert(attempts == 3);
    attempts = 0; cancelled = true;
    assert(!assembleFileWithRetry(1, nullptr, nullptr, nullptr, nullptr, isCancelled));
    assert(attempts == 0);
    cancelled = false; cancelDuringDelay = true;
    assert(!assembleFileWithRetry(1, nullptr, nullptr, nullptr, nullptr, isCancelled));
    assert(attempts == 1);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "content-retry-test.cpp"
            executable = Path(temporary) / "content-retry-test"
            test_source.write_text(program)
            subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(test_source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_initialization_error_does_not_jump_to_a_frame(self):
        repository = Path(__file__).resolve().parents[3]
        source = (repository / "engine/common/host.c").read_text()
        start = source.index("\tif( host.framecount < 3 )", source.index("void GAME_EXPORT Host_Error("))
        end = source.index("\n\tif( host.framecount == host.errorframe )", start)
        program = """
#include <assert.h>
#define S_ERROR ""
enum { HOST_INIT, HOST_FRAME };
static struct { int framecount, status; } host;
static char hosterror1[] = "Missing client library";
static int fatal, aborted;
void Sys_Error(const char *format, ...) { (void)format; fatal++; }
void Con_Printf(const char *format, ...) { (void)format; }
void Host_AbortCurrentFrame(void) { aborted++; }
void check_error(void) {
""" + source[start:end] + """
}
int main(void) {
    host.status = HOST_INIT;
    check_error();
    assert(fatal == 1 && aborted == 0);
    fatal = 0;
    host.status = HOST_FRAME;
    check_error();
    assert(fatal == 0 && aborted == 1);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "host-error-test.c"
            executable = Path(temporary) / "host-error-test"
            test_source.write_text(program)
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test_source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_launcher_actions_and_anonymous_install(self):
        repository = Path(__file__).resolve().parents[3]
        browser = (repository / "ios/Xash3D-iOS/view/FileBrowserViewController.mm").read_text()
        navigation = browser.split("self.navigationItem.rightBarButtonItems = @[", 1)[1].split("];", 1)[0]
        self.assertIn("launchBtn", navigation)
        self.assertIn("launchSpacing", navigation)
        self.assertIn("launchSpacing.width = 20;", browser)
        self.assertEqual(navigation.count("@selector(steamTapped)"), 1)
        self.assertNotIn("settingsTapped", browser)
        self.assertNotIn("map_background", browser)
        launcher = (repository / "ios/Xash3D-iOS/model/launchdialog.mm").read_text()
        self.assertNotIn('stringByAppendingPathComponent:@"svenclient"', launcher)
        self.assertIn("svenios::migrateLegacyRoot", launcher)
        defaults = launcher.split('extern "C" void IOS_SetDefaultArgs()', 1)[1].split('\n}', 1)[0]
        self.assertNotIn("map_background", defaults)
        self.assertIn('"svenclient", "-console", "-game", "svencoop"', defaults)
        self.assertNotIn("arrow.down.circle", navigation)
        self.assertIn("self.toolbarItems = @[folderBtn, flex, importBtn, flex, pasteBtn];", browser)
        self.assertIn("self.pasteButton.enabled = (_pasteboardPath != nil);", browser)
        launch = browser.split("- (void)launchTapped", 1)[1].split("- (void)steamTapped", 1)[0]
        options = (repository / "ios/Xash3D-iOS/view/LaunchOptionsViewController.inc").read_text()
        self.assertIn('setTitle:@"Cancel"', options)
        self.assertIn('setTitle:@"Launch"', options)
        self.assertIn('@"Immersive mode"', options)
        self.assertIn("UISwitch", options)
        self.assertIn("dismissViewControllerAnimated:YES completion:", options)
        self.assertLess(launch.index("launch.onLaunch ="), launch.index("doLaunchWithGameDir:"))
        self.assertIn('@"immersive_mode_svencoop": @NO', browser)
        self.assertIn('[argArray addObject:@"-noimmersive"]', browser)
        steam = (repository / "ios/Xash3D-iOS/view/SteamViewController.inc").read_text()
        self.assertIn('hasGameData:@"svencoop"] ? @"Reinstall game" : @"Install game"', steam)
        downloader = (repository / "ios/Xash3D-iOS/model/GameDataDownloader.mm").read_text()
        self.assertNotIn("requestLicense:", downloader)
        self.assertIn("[client anonymousLoginWithError:&err]", downloader)
        self.assertIn("if (!self.cancelled) purgeForeignBinaries(targetDir);", downloader)
        self.assertIn("assembleFileWithRetry(depotId, mf", downloader)
        self.assertIn("prepareForLaunch:", launch)

    def test_steam_reads_honor_deadlines_and_cancellation(self):
        repository = Path(__file__).resolve().parents[3]
        source = (repository / "ios/Xash3D-iOS/model/GameDataDownloader.mm").read_text()
        start = source.index("- (BOOL)readExact:(uint8_t *)d length:(size_t)l {")
        end = source.index("- (BOOL)connectWithError:", start)
        method = source[start:end].replace(
            "- (BOOL)readExact:(uint8_t *)d length:(size_t)l", "bool readExact(uint8_t *d, size_t l)", 1
        ).replace('@"', '"')
        program = """
#include "SteamContent.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#define YES true
#define NO false
void logToFile(const char *, ...) {}
struct CMReadTest {
    std::atomic<int> _sock{-1};
    std::atomic<bool> _closed{false};
    uint64_t _readDeadline = 0;
""" + method + """
};
int main() {
    int descriptors[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    CMReadTest reader; reader._sock = descriptors[0];
    uint8_t buffer[4];
    auto started = svensteam::milliseconds();
    reader._readDeadline = started + 80;
    assert(!reader.readExact(buffer, 1));
    assert(svensteam::milliseconds() - started < 1000);
    reader._readDeadline = svensteam::milliseconds() + 2000;
    std::thread sender([&] {
        for (uint8_t value = 1; value <= 4; ++value) {
            usleep(10000); assert(write(descriptors[1], &value, 1) == 1);
        }
    });
    assert(reader.readExact(buffer, 4)); sender.join();
    for (unsigned index = 0; index < 4; ++index) assert(buffer[index] == index + 1);
    reader._readDeadline = UINT64_MAX;
    started = svensteam::milliseconds();
    std::thread cancellation([&] { usleep(20000); reader._closed = true; });
    assert(!reader.readExact(buffer, 1)); cancellation.join();
    assert(svensteam::milliseconds() - started < 1000);
    reader._closed = false;
    close(descriptors[1]);
    assert(!reader.readExact(buffer, 1));
    close(descriptors[0]);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "SteamReadTests.cpp"
            executable = Path(temporary) / "steam-read-tests"
            test_source.write_text(program)
            subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra",
                            "-Werror", "-pthread", "-I", str(repository / "ios/Xash3D-iOS/model"),
                            str(test_source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)

    def test_steam_manifest_file_collection_syntax(self):
        repository = Path(__file__).resolve().parents[3]
        source = (repository / "ios/Xash3D-iOS/model/GameDataDownloader.mm").read_text()
        loop_start = source.index("            for (NSDictionary *f in files) {")
        loop_end = source.index("            totalFiles = allFiles.count;", loop_start)
        program = (
            '#include "SteamCMSyntaxPrelude.h"\n'
            "void collectManifestFiles(NSArray *files, NSMutableArray *allFiles, int depotId, NSDictionary *ds) {\n" +
            source[loop_start:loop_end] + "\n}\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "SteamManifestSyntax.mm"
            test_source.write_text(program)
            command = [os.environ.get("CXX", "clang++"), "-fsyntax-only", "-std=c++17",
                       "-fblocks", "-fobjc-runtime=ios-13.0", "-fobjc-arc",
                       "-Wno-objc-root-class", "-I", str(Path(__file__).resolve().parent), str(test_source)]
            subprocess.run(command, check=True)
            test_source.write_text(program.replace(
                '[allFiles addObject:', 'f = normalized;\n[allFiles addObject:', 1))
            invalid = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn("fast enumeration variables cannot be modified", invalid.stderr)

    def test_steam_cm_login_and_heartbeat_syntax(self):
        repository = Path(__file__).resolve().parents[3]
        source_path = repository / "ios/Xash3D-iOS/model/GameDataDownloader.mm"
        source = source_path.read_text()
        helpers = source[source.index("typedef std::vector<uint8_t> bytes;"):
                         source.index("static int32_t adler32_steam(")]
        interface_start = source.index("@interface SteamCMClient")
        interface_end = source.index("@end", interface_start) + len("@end")
        heartbeat = source[source.index("- (void)startHeartbeat:(int)intervalSeconds {"):
                           source.index("#pragma mark - Multi (emsg=1) message parsing")]
        login_start = source.index("- (BOOL)anonymousLoginWithError:(NSError **)error {")
        login = source[login_start:source.index("- (void)parseLogonResponseHeader:", login_start)]
        content_start = source.index("- (BOOL)waitForMessage:(int)expected job:(uint64_t)job timeout:(int)seconds body:(bytes *)response error:(NSError **)error {")
        content = source[content_start:source.index("\n@end", content_start)]
        methods = """
@interface SteamCMClient (SyntaxDependencies)
- (void)status:(NSString *)status;
- (void)startHeartbeat:(int)seconds;
- (void)stopHeartbeat;
- (void)sendProtobufMsg:(int)emsg body:(const bytes &)body header:(const bytes &)header;
- (BOOL)readMessageWithEmsg:(int *)emsg body:(bytes *)body isProto:(BOOL *)isProto
                    header:(bytes *)header error:(NSError **)error;
- (NSArray *)parseMultiMessage:(const bytes &)body;
@end
"""
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "SteamCMClientSyntax.mm"
            test_source.write_text(
                '#include "SteamCMSyntaxPrelude.h"\n' + helpers +
                source[interface_start:interface_end] + methods +
                "\n@implementation SteamCMClient\n" + heartbeat + login + content + "\n@end\n")
            subprocess.run([os.environ.get("CXX", "clang++"), "-fsyntax-only", "-std=c++17",
                            "-fblocks", "-fobjc-runtime=ios-13.0", "-fobjc-arc",
                            "-Wno-incomplete-implementation", "-Wno-objc-root-class",
                            "-I", str(Path(__file__).resolve().parent), str(test_source)], check=True)

    def test_crash_register_layouts(self):
        repository = Path(__file__).resolve().parents[3]
        source = (repository / "engine/platform/posix/crash_posix.c").read_text()
        registers = source.split("static void Sys_DumpRegisters(", 1)[1].split(
            "\n#endif // XASH_ANDROID || XASH_LINUX || XASH_APPLE", 1)[0]
        registers = "static void Sys_DumpRegisters(" + registers
        fault_start = source.index("\t\t\tucontext_t *uc =", source.index("static void Sys_Crash("))
        fault_end = source.index("\n\t\t\tSys_DumpFaultingModules(", fault_start)
        fault_registers = source[fault_start:fault_end]
        cases = (
            ("apple-arm64", 1, "__aarch64__", """
typedef struct {
    uint64_t __x[29], opaque_fp, opaque_lr, opaque_sp, opaque_pc;
    unsigned int __cpsr;
} arm_thread_state64_t;
typedef struct { arm_thread_state64_t __ss; } machine_context;
typedef struct { machine_context *uc_mcontext; } ucontext_t;
#define arm_thread_state64_get_fp(state) ((state).opaque_fp)
#define arm_thread_state64_get_lr(state) ((state).opaque_lr)
#define arm_thread_state64_get_sp(state) ((state).opaque_sp)
#define arm_thread_state64_get_pc(state) ((state).opaque_pc)
""", """
machine_context machine = {0};
ucontext_t context = { &machine };
machine.__ss.opaque_pc = 0x111;
machine.__ss.opaque_lr = 0x222;
machine.__ss.opaque_sp = 0x333;
machine.__ss.__x[28] = 0x444;
""", "pc    =0000000000000111", "0x222"),
            ("apple-x86_64", 1, "__x86_64__", """
typedef struct { uint64_t __rip, __rsp, __rbp; } thread_state;
typedef struct { thread_state __ss; } machine_context;
typedef struct { machine_context *uc_mcontext; } ucontext_t;
""", """
machine_context machine = {0};
ucontext_t context = { &machine };
machine.__ss.__rip = 0x111;
machine.__ss.__rsp = 0x333;
""", "rip=0000000000000111", "0"),
            ("linux-arm64", 0, "__aarch64__", """
typedef struct { uint64_t regs[31], sp, pc, pstate; } machine_context;
typedef struct { machine_context uc_mcontext; } ucontext_t;
""", """
ucontext_t context = {0};
context.uc_mcontext.pc = 0x111;
context.uc_mcontext.regs[30] = 0x222;
context.uc_mcontext.sp = 0x333;
""", "pc    =0000000000000111", "0x222"),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, apple, architecture, declarations, setup, expected, link_register in cases:
                with self.subTest(platform=name):
                    program = """
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#undef __aarch64__
#undef __arm__
#undef __x86_64__
static int Sys_CrashAppend(char *message, int len, size_t max_len, int logfd, const char *format, ...)
{
    (void)logfd;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(message + len, max_len - len, format, arguments);
    va_end(arguments);
    return len + written;
}
"""
                    program += f"\n#define XASH_APPLE {apple}\n#define {architecture} 1\n"
                    program += declarations + registers
                    program += "\nstatic void check_fault_registers(void *context) {\n" + fault_registers
                    program += f"\nassert(pc == 0x111 && lr == {link_register} && sp == 0x333);\n}}\n"
                    program += "int main(void) {\n" + setup
                    program += """
char message[4096] = {0};
int len = 0;
Sys_DumpRegisters(NULL, message, &len, sizeof(message), -1);
assert(len == 0);
Sys_DumpRegisters(&context, message, &len, sizeof(message), -1);
assert(len > 0);
check_fault_registers(&context);
"""
                    program += f'assert(strstr(message, "{expected}") != NULL);\n'
                    if apple:
                        program += """
context.uc_mcontext = NULL;
int previous = len;
Sys_DumpRegisters(&context, message, &len, sizeof(message), -1);
assert(len == previous);
"""
                    program += "return 0;\n}\n"
                    test_source = root / f"{name}.c"
                    executable = root / name
                    test_source.write_text(program)
                    subprocess.run([os.environ.get("CC", "clang"), "-std=gnu11", "-Wall", "-Wextra",
                                    "-Werror", str(test_source), "-o", str(executable)], check=True)
                    subprocess.run([str(executable)], check=True)

    def test_shared_ci_environment_with_nounset(self):
        repository = Path(__file__).resolve().parents[3]
        environment = dict(os.environ)
        for name in ("TRAVIS_BUILD_DIR", "TRAVIS_CPU_ARCH", "GITHUB_WORKSPACE", "GH_CPU_ARCH", "BUILDDIR", "ARCH"):
            environment.pop(name, None)
        cases = (
            ({"GITHUB_WORKSPACE": "/github/workspace", "GH_CPU_ARCH": "arm64"}, "/github/workspace:arm64"),
            ({"TRAVIS_BUILD_DIR": "/travis/workspace", "TRAVIS_CPU_ARCH": "amd64",
              "GITHUB_WORKSPACE": "/github/workspace", "GH_CPU_ARCH": "arm64"}, "/travis/workspace:amd64"),
            ({}, ":"),
        )
        for variables, expected in cases:
            with self.subTest(variables=variables):
                result = subprocess.run(
                    ["bash", "-euo", "pipefail", "-c",
                     '. scripts/lib.sh; printf "%s:%s" "${BUILDDIR:-}" "${ARCH:-}"'],
                    cwd=repository, env=dict(environment, **variables), check=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                )
                self.assertEqual(result.stdout, expected)

    def test_apple_static_vgui_link_options(self):
        repository = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            overrides = repository / "scripts/ios/hlsdk_overrides.cmake"
            (root / "client.cpp").write_text("int client_symbol() { return 1; }\n")
            (root / "vgui.cpp").write_text("int vgui_symbol() { return 1; }\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.19)\n"
                "project(SvenLinkTest LANGUAGES CXX)\n"
                "set(CMAKE_SYSTEM_NAME iOS)\nset(USE_STATIC_FREEVGUI ON)\n"
                f'include("{overrides}")\n'
                "add_library(vgui STATIC vgui.cpp)\nadd_library(client SHARED client.cpp)\n"
                "target_link_libraries(client PRIVATE -Wl,--whole-archive vgui -Wl,--no-whole-archive)\n"
                'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/link-properties.txt" '
                'CONTENT "$<TARGET_PROPERTY:client,LINK_LIBRARIES>\\n$<TARGET_PROPERTY:client,LINK_OPTIONS>\\n$<TARGET_FILE_NAME:client>")\n'
            )
            subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            properties = (root / "build/link-properties.txt").read_text()
            self.assertNotIn("--whole-archive", properties)
            self.assertNotIn("--no-whole-archive", properties)
            self.assertIn("LINKER:-force_load,", properties)
            self.assertIn("vgui", properties)
            self.assertIn("client_ios_arm64", properties)
            self.assertNotIn("libclient_ios_arm64", properties)

    def test_apple_hlsdk_c_inline_linkage(self):
        repository = Path(__file__).resolve().parents[3]
        overrides = repository / "scripts/ios/hlsdk_overrides.cmake"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "cl_dll").mkdir()
            (root / "pm_shared").mkdir()
            (root / "pm_shared/pm_math.c").write_text(
                "inline void sincos_ps(float value, float *sine, float *cosine) "
                "{ *sine = value; *cosine = value + 1; }\n"
                "float angle_matrix_test(void) { float sine, cosine; "
                "sincos_ps(1, &sine, &cosine); return sine + cosine; }\n"
            )
            (root / "client.cpp").write_text(
                'extern "C" float angle_matrix_test(void);\n'
                "int main() { return angle_matrix_test() == 3 ? 0 : 1; }\n"
            )
            (root / "cl_dll/CMakeLists.txt").write_text(
                "add_library(client STATIC ../pm_shared/pm_math.c)\n"
                "target_compile_options(client PRIVATE -O0 -std=c11)\n"
            )
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.19)\n"
                "project(SvenInlineTest LANGUAGES C CXX)\n"
                "set(CMAKE_SYSTEM_NAME iOS)\n"
                f'include("{overrides}")\n'
                "add_subdirectory(cl_dll)\n"
                "add_executable(inline_test client.cpp)\n"
                "target_link_libraries(inline_test PRIVATE client)\n"
            )
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build),
                            "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            subprocess.run(["cmake", "--build", str(build)],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            subprocess.run([str(build / "inline_test")], check=True)

    def test_only_sven_client_is_built_and_packaged(self):
        repository = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = root / "scripts" / "ios"
            scripts.mkdir(parents=True)
            for name in ("buildhlsdk.sh", "prepare_game_libs.sh", "buildallmods.sh"):
                shutil.copyfile(repository / "scripts" / "ios" / name, scripts / name)
            executables = root / "bin"
            executables.mkdir()
            mock = """#!/usr/bin/env python3
import os, pathlib, sys
with open(os.environ['SVEN_TEST_LOG'], 'a') as log:
    log.write(pathlib.Path(sys.argv[0]).name + ' ' + ' '.join(sys.argv[1:]) + '\\n')
if pathlib.Path(sys.argv[0]).name == 'git':
    pathlib.Path(sys.argv[-1], '.git').mkdir(parents=True)
elif '--build' in sys.argv:
    output = pathlib.Path(os.environ['SVEN_TEST_LIBRARY'])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b'client')
"""
            for name in ("git", "cmake"):
                command = executables / name
                command.write_text(mock)
                command.chmod(0o755)
            log = root / "commands.log"
            library = root / "build/ios/libs/svencoop/cl_dlls/client_ios_arm64.dylib"
            library.parent.mkdir(parents=True)
            stale_library = library.with_name("client_arm64.dylib")
            stale_library.write_bytes(b"obsolete")
            environment = dict(os.environ, PATH=str(executables) + os.pathsep + os.environ["PATH"],
                               SVEN_TEST_LOG=str(log), SVEN_TEST_LIBRARY=str(library))
            subprocess.run(["bash", str(scripts / "buildallmods.sh")], env=environment, check=True)
            commands = log.read_text()
            self.assertIn("--branch svencoop --single-branch", commands)
            self.assertIn("https://github.com/hasandramali/hlsdk-portable", commands)
            self.assertIn("-DGAMEDIR=svencoop -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON", commands)
            self.assertIn("-DFREEVGUI_SRC_DIR=", commands)
            self.assertIn("-DCMAKE_PROJECT_INCLUDE=", commands)
            self.assertNotIn("cs16", commands)
            self.assertNotIn("opfor", commands)
            self.assertTrue(library.is_file())
            self.assertFalse(stale_library.exists())
            products = root / "products"
            environment.update(BUILT_PRODUCTS_DIR=str(products), EXECUTABLE_FOLDER_PATH="SvenClient.app")
            subprocess.run(["bash", str(scripts / "prepare_game_libs.sh")], env=environment, check=True)
            self.assertTrue((products / "SvenClient.app/svencoop/cl_dlls/client_ios_arm64.dylib").is_file())
            self.assertEqual(commands, log.read_text())


if __name__ == "__main__":
    unittest.main()
