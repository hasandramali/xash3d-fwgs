#include "../../../ios/Xash3D-iOS/model/IOSPaths.hpp"
#include <cassert>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

static std::string readFile(const fs::path &path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
    assert(argc == 2);
    fs::path root = fs::path(argv[1]) / "documents";
    std::string message;
    assert(svenios::migrateLegacyRoot(root, message));
    assert(fs::is_directory(root));
    writeFile(root / "svenclient/svencoop/maps/test.bsp", "map-data");
    writeFile(root / "svenclient/xash_ios.log", "legacy-log");
    assert(svenios::migrateLegacyRoot(root, message));
    assert(readFile(root / "svencoop/maps/test.bsp") == "map-data");
    assert(readFile(root / "xash_ios.log") == "legacy-log");
    assert(!fs::exists(root / "svenclient"));
    assert(!fs::exists(root / ".svenclient-legacy"));
    assert(svenios::migrateLegacyRoot(root, message));
    writeFile(root / "svenclient/svencoop/maps/test.bsp", "duplicate-map");
    writeFile(root / "svenclient/svencoop/maps/second.bsp", "second-map");
    writeFile(root / ".svenclient-legacy/keep.txt", "existing-backup");
    fs::path outside = fs::path(argv[1]) / "outside";
    writeFile(outside / "keep.txt", "outside-data");
    fs::create_directory_symlink(outside, root / "svenclient/svencoop/external");
    assert(svenios::migrateLegacyRoot(root, message));
    assert(!message.empty());
    assert(!fs::exists(root / "svenclient"));
    assert(readFile(root / "svencoop/maps/test.bsp") == "map-data");
    assert(readFile(root / "svencoop/maps/second.bsp") == "second-map");
    assert(readFile(root / ".svenclient-legacy-1/svencoop/maps/test.bsp") == "duplicate-map");
    assert(readFile(root / ".svenclient-legacy/keep.txt") == "existing-backup");
    assert(fs::is_symlink(fs::symlink_status(root / "svencoop/external")));
    assert(readFile(outside / "keep.txt") == "outside-data");
    std::cout << "PASS: iOS Documents migration, file preservation, duplicate backups and symlink safety\n";
}
