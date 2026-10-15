import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SvenBuildScriptsTest(unittest.TestCase):
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
