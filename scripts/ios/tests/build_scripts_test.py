import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SvenBuildScriptsTest(unittest.TestCase):
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
        self.assertIn('actionWithTitle:@"Cancel"', launch)
        self.assertIn('actionWithTitle:@"Launch"', launch)
        self.assertLess(launch.index('actionWithTitle:@"Launch"'), launch.index("doLaunchWithGameDir:"))
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
