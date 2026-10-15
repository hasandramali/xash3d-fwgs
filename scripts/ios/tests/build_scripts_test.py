import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SvenBuildScriptsTest(unittest.TestCase):
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
                'CONTENT "$<TARGET_PROPERTY:client,LINK_LIBRARIES>\\n$<TARGET_PROPERTY:client,LINK_OPTIONS>")\n'
            )
            subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            properties = (root / "build/link-properties.txt").read_text()
            self.assertNotIn("--whole-archive", properties)
            self.assertNotIn("--no-whole-archive", properties)
            self.assertIn("LINKER:-force_load,", properties)
            self.assertIn("vgui", properties)

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
            library = root / "build/ios/libs/svencoop/cl_dlls/client.dylib"
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
            self.assertTrue(library.with_name("client_arm64.dylib").is_file())
            products = root / "products"
            environment.update(BUILT_PRODUCTS_DIR=str(products), EXECUTABLE_FOLDER_PATH="SvenClient.app")
            subprocess.run(["bash", str(scripts / "prepare_game_libs.sh")], env=environment, check=True)
            self.assertTrue((products / "SvenClient.app/svencoop/cl_dlls/client_arm64.dylib").is_file())
            self.assertEqual(commands, log.read_text())


if __name__ == "__main__":
    unittest.main()
