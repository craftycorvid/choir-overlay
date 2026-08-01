// Tests for the XDG autostart toggle behind the settings checkbox.
//
// The desktop entry's presence IS the state, so the risks are all filesystem-shaped:
// enabling must create parent dirs and write an ABSOLUTE, correctly-quoted Exec;
// disabling must remove the file; and reading must honour an entry that exists but is
// disabled in place — X-GNOME-Autostart-enabled=false (how packaging/choir.desktop
// ships) or Hidden=true (what KDE writes when you untick a startup app).
//
// No Qt; std::filesystem + a unique temp dir.

#include "config/autostart.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

using namespace choir;
namespace fs = std::filesystem;

namespace {

std::string read_all(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_all(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::trunc) << text;
}

bool has_line(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos ||
           text.compare(0, line.size() + 1, line + "\n") == 0;
}

void test_absent_is_disabled(const fs::path& tmp) {
    assert(!autostart_enabled((tmp / "nope.desktop").string()));
}

void test_enable_then_disable(const fs::path& tmp) {
    // Nested under a dir that does NOT exist yet: ~/.config/autostart is often absent.
    const std::string path = (tmp / "autostart" / "choir.desktop").string();

    assert(set_autostart(path, true, "/home/u/.local/bin/choir"));
    assert(autostart_enabled(path));

    const std::string text = read_all(path);
    assert(has_line(text, "[Desktop Entry]"));
    assert(has_line(text, "Type=Application"));
    assert(has_line(text, "Exec=/home/u/.local/bin/choir"));
    assert(has_line(text, "Icon=choir"));
    assert(has_line(text, "X-GNOME-Autostart-enabled=true"));

    assert(set_autostart(path, false, "/home/u/.local/bin/choir"));
    assert(!fs::exists(path));
    assert(!autostart_enabled(path));

    // Disabling an already-absent entry is not an error (idempotent).
    assert(set_autostart(path, false, "/home/u/.local/bin/choir"));
}

void test_exec_with_spaces_is_quoted(const fs::path& tmp) {
    const std::string path = (tmp / "spaced.desktop").string();
    assert(set_autostart(path, true, "/home/u/My Apps/choir"));
    assert(has_line(read_all(path), "Exec=\"/home/u/My Apps/choir\""));
}

void test_disabled_in_place_reads_as_off(const fs::path& tmp) {
    const std::string gnome = (tmp / "gnome-off.desktop").string();
    write_all(gnome,
              "[Desktop Entry]\nType=Application\nExec=/usr/bin/choir\n"
              "X-GNOME-Autostart-enabled=false\n");
    assert(fs::exists(gnome));
    assert(!autostart_enabled(gnome));

    const std::string kde = (tmp / "kde-off.desktop").string();
    write_all(kde, "[Desktop Entry]\nType=Application\nExec=/usr/bin/choir\nHidden=true\n");
    assert(!autostart_enabled(kde));

    // A plain entry with neither key is enabled.
    const std::string plain = (tmp / "plain.desktop").string();
    write_all(plain, "[Desktop Entry]\nType=Application\nExec=/usr/bin/choir\n");
    assert(autostart_enabled(plain));

    // A key that merely starts with "Hidden" must not disable it.
    const std::string decoy = (tmp / "decoy.desktop").string();
    write_all(decoy, "[Desktop Entry]\nExec=/usr/bin/choir\nHiddenWhenNoTray=true\n");
    assert(autostart_enabled(decoy));
}

// Re-enabling over an existing entry must replace it, not append to it.
void test_rewrite_truncates(const fs::path& tmp) {
    const std::string path = (tmp / "rewrite.desktop").string();
    assert(set_autostart(path, true, "/old/choir"));
    assert(set_autostart(path, true, "/new/choir"));
    const std::string text = read_all(path);
    assert(has_line(text, "Exec=/new/choir"));
    assert(text.find("/old/choir") == std::string::npos);
}

}  // namespace

int main() {
    const fs::path tmp =
        fs::temp_directory_path() / ("choir_test_autostart_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    test_absent_is_disabled(tmp);
    test_enable_then_disable(tmp);
    test_exec_with_spaces_is_quoted(tmp);
    test_disabled_in_place_reads_as_off(tmp);
    test_rewrite_truncates(tmp);

    fs::remove_all(tmp);
    return 0;
}
