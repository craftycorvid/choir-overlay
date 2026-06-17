#pragma once

// Settings window (Task 11).
//
// A QWidget settings dialog backed by choir::Config (config_path()):
//   - appearance: anchor combo, scale + opacity sliders, show-all-members
//     checkbox, toast anchor + duration
//   - denylist editor (one glob per line)
//   - auth: mode selector (Streamkit / OwnApp) + client_id / client_secret
//   - "Authorize with Discord" button -> emits authorize_requested()
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
class QLineEdit;
class QPlainTextEdit;
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

    // Emitted by the "Authorize with Discord" button.
    void authorize_requested();

private slots:
    void on_save_clicked();

private:
    void build_ui();
    void load_into_widgets();
    Config gather_from_widgets() const;

    Config cfg_;

    QComboBox* anchor_ = nullptr;
    QSlider* scale_ = nullptr;
    QSlider* opacity_ = nullptr;
    QCheckBox* show_all_ = nullptr;
    QComboBox* toast_anchor_ = nullptr;
    QSpinBox* toast_duration_ = nullptr;

    QPlainTextEdit* denylist_ = nullptr;

    QComboBox* auth_mode_ = nullptr;
    QLineEdit* client_id_ = nullptr;
    QLineEdit* client_secret_ = nullptr;
};

}  // namespace choir
