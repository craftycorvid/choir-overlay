#pragma once

// Settings window (Task 11).
//
// A QWidget settings dialog backed by choir::Config (config_path()):
//   - appearance: anchor combo, scale slider, HDR nits spinbox, show-all-members
//     checkbox, toast anchor + duration
//   - denylist editor (one glob per line)
//   - start-on-login checkbox (an XDG autostart entry, NOT part of Config)
//   - overlay-libraries status + install button, shown ONLY under an AppImage, which
//     has to place the injected .so files in ~/.local before any game can load them
//     (see config/backends.hpp). This is also the way back for someone who declined
//     the first-run prompt, since that answer is remembered.
//
// Saving persists to config_path() and emits config_changed(Config) so main()
// can push the new appearance into OverlayState and rebuild the Denylist.
//
// Qt-using; compiled into the `choir` executable, never libchoir_host_core.

#include "config/config.hpp"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QSlider;
class QCheckBox;
class QSpinBox;
class QPlainTextEdit;
class QShowEvent;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

namespace choir {

class SettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWindow(Config initial, QWidget* parent = nullptr);

    // The current in-memory config (reflects the last applied/saved state).
    const Config& config() const { return cfg_; }

signals:
    // Emitted after Save: the new config has been written to config_path().
    void config_changed(const choir::Config& cfg);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void on_save_clicked();
    void on_install_backends_clicked();

private:
    void build_ui();
    void load_into_widgets();
    Config gather_from_widgets() const;

    // Re-reads whether the AppImage payload is installed in ~/.local and updates the
    // status label + button. No-op when not running from an AppImage (the row is
    // never built, so backend_status_ stays null).
    void refresh_backend_row();

    Config cfg_;

    QComboBox* anchor_ = nullptr;
    QSlider* scale_ = nullptr;
    QSpinBox* hdr_nits_ = nullptr;
    QCheckBox* show_all_ = nullptr;
    QComboBox* toast_anchor_ = nullptr;
    QSpinBox* toast_duration_ = nullptr;

    QCheckBox* autostart_ = nullptr;

    // AppImage installs only; null otherwise (see config/backends.hpp).
    QLabel* backend_status_ = nullptr;
    QPushButton* backend_install_ = nullptr;

    QPlainTextEdit* denylist_ = nullptr;
};

}  // namespace choir
