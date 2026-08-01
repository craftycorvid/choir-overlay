#include "ui/settings_window.hpp"

#include "config/autostart.hpp"
#include "ipc/paths.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>

namespace choir {

namespace {

int anchor_to_index(Anchor a) { return static_cast<int>(a); }
Anchor index_to_anchor(int i) {
    if (i < 0 || i > static_cast<int>(Anchor::CenterRight)) i = static_cast<int>(Anchor::TopRight);
    return static_cast<Anchor>(i);
}

// Order MUST match the Anchor enum (TopLeft, TopRight, BottomLeft, BottomRight,
// CenterLeft, CenterRight).
void fill_anchor_combo(QComboBox* c) {
    c->addItem(QStringLiteral("Top Left"));
    c->addItem(QStringLiteral("Top Right"));
    c->addItem(QStringLiteral("Bottom Left"));
    c->addItem(QStringLiteral("Bottom Right"));
    c->addItem(QStringLiteral("Center Left"));
    c->addItem(QStringLiteral("Center Right"));
}

}  // namespace

SettingsWindow::SettingsWindow(Config initial, QWidget* parent)
    : QWidget(parent), cfg_(std::move(initial)) {
    setWindowTitle(QStringLiteral("Choir — Settings"));
    build_ui();
    load_into_widgets();
}

void SettingsWindow::build_ui() {
    auto* root = new QVBoxLayout(this);

    // --- Startup (an XDG autostart entry, applied on Save like everything else) ---
    autostart_ = new QCheckBox(QStringLiteral("Start Choir on login"), this);
    autostart_->setToolTip(QStringLiteral(
        "Writes an autostart entry to ~/.config/autostart/choir.desktop."));
    root->addWidget(autostart_);

    // --- Appearance ---
    auto* appearance = new QGroupBox(QStringLiteral("Appearance"), this);
    auto* af = new QFormLayout(appearance);

    anchor_ = new QComboBox(appearance);
    fill_anchor_combo(anchor_);
    af->addRow(QStringLiteral("Panel anchor"), anchor_);

    scale_ = new QSlider(Qt::Horizontal, appearance);
    scale_->setRange(50, 300);  // 0.50x .. 3.00x
    af->addRow(QStringLiteral("Scale (%)"), scale_);

    hdr_nits_ = new QSpinBox(appearance);
    hdr_nits_->setRange(80, 1000);
    hdr_nits_->setSingleStep(10);
    hdr_nits_->setSuffix(QStringLiteral(" nits"));
    hdr_nits_->setToolTip(QStringLiteral(
        "Overlay paper-white brightness on HDR displays (no effect in SDR). "
        "Applies on the next game launch."));
    af->addRow(QStringLiteral("HDR brightness"), hdr_nits_);

    show_all_ = new QCheckBox(QStringLiteral("Show all members (not just speakers)"),
                              appearance);
    af->addRow(QString(), show_all_);

    toast_anchor_ = new QComboBox(appearance);
    fill_anchor_combo(toast_anchor_);
    af->addRow(QStringLiteral("Toast anchor"), toast_anchor_);

    toast_duration_ = new QSpinBox(appearance);
    toast_duration_->setRange(500, 60000);
    toast_duration_->setSingleStep(500);
    toast_duration_->setSuffix(QStringLiteral(" ms"));
    af->addRow(QStringLiteral("Toast duration"), toast_duration_);

    root->addWidget(appearance);

    // --- Denylist ---
    auto* dl_box = new QGroupBox(QStringLiteral("Denylist (one glob per line)"), this);
    auto* dl_layout = new QVBoxLayout(dl_box);
    denylist_ = new QPlainTextEdit(dl_box);
    denylist_->setPlaceholderText(QStringLiteral("steam\n*launcher*\nobs"));
    dl_layout->addWidget(denylist_);
    root->addWidget(dl_box);

    // --- Buttons ---
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* save = new QPushButton(QStringLiteral("Save"), this);
    connect(save, &QPushButton::clicked, this, &SettingsWindow::on_save_clicked);
    buttons->addWidget(save);
    root->addLayout(buttons);
}

void SettingsWindow::load_into_widgets() {
    autostart_->setChecked(autostart_enabled(autostart_path()));

    const AppearanceConfig& a = cfg_.appearance;
    anchor_->setCurrentIndex(anchor_to_index(a.anchor));
    scale_->setValue(static_cast<int>(a.scale * 100.0f + 0.5f));
    hdr_nits_->setValue(static_cast<int>(a.hdr_nits + 0.5f));
    show_all_->setChecked(a.show_all_members);
    toast_anchor_->setCurrentIndex(anchor_to_index(a.toast_anchor));
    toast_duration_->setValue(a.toast_duration_ms);

    QStringList lines;
    for (const auto& pat : cfg_.denylist) lines << QString::fromStdString(pat);
    denylist_->setPlainText(lines.join(QChar('\n')));
}

Config SettingsWindow::gather_from_widgets() const {
    Config c = cfg_;  // preserve tokens we don't edit here

    AppearanceConfig& a = c.appearance;
    a.anchor = index_to_anchor(anchor_->currentIndex());
    a.scale = static_cast<float>(scale_->value()) / 100.0f;
    a.hdr_nits = static_cast<float>(hdr_nits_->value());
    a.show_all_members = show_all_->isChecked();
    a.toast_anchor = index_to_anchor(toast_anchor_->currentIndex());
    a.toast_duration_ms = toast_duration_->value();

    c.denylist.clear();
    const QStringList lines = denylist_->toPlainText().split(QChar('\n'));
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) c.denylist.push_back(trimmed.toStdString());
    }

    return c;
}

void SettingsWindow::on_save_clicked() {
    cfg_ = gather_from_widgets();
    cfg_.save(config_path());

    // Autostart lives on the filesystem, not in cfg_. If the write is refused, snap the
    // box back to the truth rather than leaving the UI claiming something that isn't so.
    set_autostart(autostart_path(), autostart_->isChecked(),
                  QCoreApplication::applicationFilePath().toStdString());
    autostart_->setChecked(autostart_enabled(autostart_path()));

    emit config_changed(cfg_);
}

void SettingsWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // The window outlives each open, and the autostart entry can change behind us
    // (install-user.sh --autostart, or the DE's own startup-apps UI). Re-read it.
    autostart_->setChecked(autostart_enabled(autostart_path()));
}

}  // namespace choir
