// Tests for the AppImage backend install (host/config/backends.hpp).
//
// The AppImage carries libchoir_overlay.so + libchoir_gl.so as payload and has to
// place them somewhere the rest of the system can reach, because neither can be
// loaded from inside the image: the Vulkan loader reads library_path from a game
// process, and choir-run LD_PRELOADs into a game, both long after the AppImage's
// /tmp/.mount_XXXXXX is gone.
//
// The risks are all filesystem-shaped, and two of them are load-bearing:
//   - the manifest must be written LAST, so a failed install never leaves a manifest
//     pointing at a missing .so (that error is logged by every Vulkan app on the box)
//   - re-installing must replace the .so via rename, not truncate-in-place, or an
//     update while a game holds the old one mapped fails with ETXTBSY
//
// No Qt; std::filesystem + a unique temp dir standing in for $HOME.

#include "config/backends.hpp"
#include "ipc/paths.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include <nlohmann/json.hpp>

using namespace choir;
namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_all(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// A source dir shaped like the AppImage's usr/lib/choir.
void seed_payload(const fs::path& src, const std::string& tag) {
    write_all(src / "libchoir_overlay.so", "VK-LAYER-" + tag);
    write_all(src / "libchoir_gl.so", "GL-INTERPOSER-" + tag);
}

void test_appimage_dir_reads_env() {
    // Empty means "not running from an AppImage", which is what makes every caller
    // a no-op for source/pacman installs.
    ::unsetenv("APPDIR");
    assert(appimage_dir().empty());
    // Empty payload dir is the "nothing to do" signal every caller keys off; it must
    // NOT degrade to the relative path "/usr/lib/choir".
    assert(appimage_payload_dir().empty());

    ::setenv("APPDIR", "/tmp/.mount_Choir1234", 1);
    assert(appimage_dir() == "/tmp/.mount_Choir1234");
    assert(appimage_payload_dir() == "/tmp/.mount_Choir1234/usr/lib/choir");
    ::unsetenv("APPDIR");
}

void test_install_writes_libs_manifest_and_wrapper(const fs::path& tmp) {
    const fs::path src = tmp / "appdir" / "usr" / "lib" / "choir";
    seed_payload(src, "v1");

    assert(install_backends(src.string()));

    // Both payload libs land in ~/.local/lib/choir with their bytes intact.
    const fs::path lib_dir = backend_lib_dir();
    assert(read_all(lib_dir / "libchoir_overlay.so") == "VK-LAYER-v1");
    assert(read_all(lib_dir / "libchoir_gl.so") == "GL-INTERPOSER-v1");

    // The implicit-layer manifest must be valid JSON pointing at the ABSOLUTE
    // installed path — a relative one would resolve against the manifest dir.
    const std::string manifest_text = read_all(vulkan_manifest_path());
    const auto j = nlohmann::json::parse(manifest_text, nullptr, false);
    assert(!j.is_discarded());
    assert(j["layer"]["library_path"] ==
           (lib_dir / "libchoir_overlay.so").string());
    assert(j["layer"]["name"] == "VK_LAYER_choir_overlay_x86_64");
    assert(j["layer"]["type"] == "GLOBAL");
    // The kill switch both backends honour must survive into the generated manifest.
    assert(j["layer"]["disable_environment"]["DISABLE_CHOIR_OVERLAY"] == "1");

    // choir-run has to be executable, and must name the installed GL lib.
    const std::string wrapper = read_all(gl_wrapper_path());
    assert(wrapper.find((lib_dir / "libchoir_gl.so").string()) != std::string::npos);
    assert(wrapper.compare(0, 2, "#!") == 0);
    const auto perms = fs::status(gl_wrapper_path()).permissions();
    assert((perms & fs::perms::owner_exec) != fs::perms::none);
    assert((perms & fs::perms::group_exec) != fs::perms::none);
    assert((perms & fs::perms::others_exec) != fs::perms::none);
}

void test_up_to_date_tracks_payload(const fs::path& tmp) {
    const fs::path src = tmp / "sync" / "lib";
    seed_payload(src, "a");

    const std::string lib_dir = backend_lib_dir();
    // Nothing installed from THIS payload yet.
    fs::remove_all(lib_dir);
    assert(!backends_up_to_date(src.string(), lib_dir));

    assert(install_backends(src.string()));
    assert(backends_up_to_date(src.string(), lib_dir));

    // A rebuilt AppImage ships different bytes at the same size -> must re-sync.
    seed_payload(src, "b");
    assert(!backends_up_to_date(src.string(), lib_dir));

    assert(install_backends(src.string()));
    assert(backends_up_to_date(src.string(), lib_dir));
    assert(read_all(fs::path(lib_dir) / "libchoir_overlay.so") == "VK-LAYER-b");

    // Only one of the two changing is still out of date.
    write_all(src / "libchoir_gl.so", "GL-INTERPOSER-c");
    assert(!backends_up_to_date(src.string(), lib_dir));
}

// An update while a game has the old .so mapped must not fail: replacing via rename
// leaves the running process on its old inode instead of hitting ETXTBSY.
void test_reinstall_replaces_a_busy_file(const fs::path& tmp) {
    const fs::path src = tmp / "busy" / "lib";
    seed_payload(src, "old");
    assert(install_backends(src.string()));

    const fs::path installed = fs::path(backend_lib_dir()) / "libchoir_overlay.so";
    // Hold the installed file open, standing in for a game holding it mapped.
    std::ifstream holder(installed, std::ios::binary);
    assert(holder.good());

    seed_payload(src, "new");
    assert(install_backends(src.string()));
    assert(read_all(installed) == "VK-LAYER-new");
}

// If the payload is missing, no manifest may be left behind: a manifest naming an
// absent library makes the loader complain in every Vulkan application.
void test_missing_payload_leaves_no_manifest(const fs::path& tmp) {
    fs::remove(vulkan_manifest_path());
    assert(!install_backends((tmp / "does-not-exist").string()));
    assert(!fs::exists(vulkan_manifest_path()));

    // Half a payload is still a failure, and still writes no manifest.
    const fs::path partial = tmp / "partial" / "lib";
    write_all(partial / "libchoir_overlay.so", "VK-LAYER-only");
    assert(!install_backends(partial.string()));
    assert(!fs::exists(vulkan_manifest_path()));
}

}  // namespace

int main() {
    const fs::path tmp =
        fs::temp_directory_path() / ("choir_test_backends_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Redirect every install destination into the temp tree. backend_lib_dir() and
    // gl_wrapper_path() hang off $HOME; vulkan_manifest_path() off data_home().
    const fs::path home = tmp / "home";
    ::setenv("HOME", home.c_str(), 1);
    ::unsetenv("XDG_DATA_HOME");

    test_appimage_dir_reads_env();
    assert(backend_lib_dir() == (home / ".local" / "lib" / "choir").string());
    assert(gl_wrapper_path() == (home / ".local" / "bin" / "choir-run").string());

    test_install_writes_libs_manifest_and_wrapper(tmp);
    test_up_to_date_tracks_payload(tmp);
    test_reinstall_replaces_a_busy_file(tmp);
    test_missing_payload_leaves_no_manifest(tmp);

    fs::remove_all(tmp);
    return 0;
}
