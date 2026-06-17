#include "ui/settings_window.hpp"

#include "ipc/paths.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
    if (i < 0 || i > 3) i = static_cast<int>(Anchor::TopRight);
    return static_cast<Anchor>(i);
}

void fill_anchor_combo(QComboBox* c) {
    c->addItem(QStringLiteral("Top Left"));
    c->addItem(QStringLiteral("Top Right"));
    c->addItem(QStringLiteral("Bottom Left"));
    c->addItem(QStringLiteral("Bottom Right"));
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

    // --- Appearance ---
    auto* appearance = new QGroupBox(QStringLiteral("Appearance"), this);
    auto* af = new QFormLayout(appearance);

    anchor_ = new QComboBox(appearance);
    fill_anchor_combo(anchor_);
    af->addRow(QStringLiteral("Panel anchor"), anchor_);

    scale_ = new QSlider(Qt::Horizontal, appearance);
    scale_->setRange(50, 300);  // 0.50x .. 3.00x
    af->addRow(QStringLiteral("Scale (%)"), scale_);

    opacity_ = new QSlider(Qt::Horizontal, appearance);
    opacity_->setRange(10, 100);  // 0.10 .. 1.00
    af->addRow(QStringLiteral("Opacity (%)"), opacity_);

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

    // --- Auth ---
    auto* auth = new QGroupBox(QStringLiteral("Discord authentication"), this);
    auto* auf = new QFormLayout(auth);

    auth_mode_ = new QComboBox(auth);
    auth_mode_->addItem(QStringLiteral("Streamkit (zero setup)"));
    auth_mode_->addItem(QStringLiteral("Own app"));
    auf->addRow(QStringLiteral("Mode"), auth_mode_);

    client_id_ = new QLineEdit(auth);
    auf->addRow(QStringLiteral("Client ID"), client_id_);

    client_secret_ = new QLineEdit(auth);
    client_secret_->setEchoMode(QLineEdit::Password);
    auf->addRow(QStringLiteral("Client secret"), client_secret_);

    auto* authorize = new QPushButton(QStringLiteral("Authorize with Discord"), auth);
    auf->addRow(QString(), authorize);
    connect(authorize, &QPushButton::clicked, this, &SettingsWindow::authorize_requested);

    root->addWidget(auth);

    // --- Buttons ---
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* save = new QPushButton(QStringLiteral("Save"), this);
    connect(save, &QPushButton::clicked, this, &SettingsWindow::on_save_clicked);
    buttons->addWidget(save);
    root->addLayout(buttons);
}

void SettingsWindow::load_into_widgets() {
    const AppearanceConfig& a = cfg_.appearance;
    anchor_->setCurrentIndex(anchor_to_index(a.anchor));
    scale_->setValue(static_cast<int>(a.scale * 100.0f + 0.5f));
    opacity_->setValue(static_cast<int>(a.opacity * 100.0f + 0.5f));
    show_all_->setChecked(a.show_all_members);
    toast_anchor_->setCurrentIndex(anchor_to_index(a.toast_anchor));
    toast_duration_->setValue(a.toast_duration_ms);

    QStringList lines;
    for (const auto& pat : cfg_.denylist) lines << QString::fromStdString(pat);
    denylist_->setPlainText(lines.join(QChar('\n')));

    auth_mode_->setCurrentIndex(cfg_.auth_mode == AuthMode::OwnApp ? 1 : 0);
    client_id_->setText(QString::fromStdString(cfg_.client_id));
    client_secret_->setText(QString::fromStdString(cfg_.client_secret));
}

Config SettingsWindow::gather_from_widgets() const {
    Config c = cfg_;  // preserve tokens we don't edit here

    AppearanceConfig& a = c.appearance;
    a.anchor = index_to_anchor(anchor_->currentIndex());
    a.scale = static_cast<float>(scale_->value()) / 100.0f;
    a.opacity = static_cast<float>(opacity_->value()) / 100.0f;
    a.show_all_members = show_all_->isChecked();
    a.toast_anchor = index_to_anchor(toast_anchor_->currentIndex());
    a.toast_duration_ms = toast_duration_->value();

    c.denylist.clear();
    const QStringList lines = denylist_->toPlainText().split(QChar('\n'));
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) c.denylist.push_back(trimmed.toStdString());
    }

    c.auth_mode = auth_mode_->currentIndex() == 1 ? AuthMode::OwnApp : AuthMode::Streamkit;
    c.client_id = client_id_->text().trimmed().toStdString();
    c.client_secret = client_secret_->text().toStdString();

    return c;
}

void SettingsWindow::on_save_clicked() {
    cfg_ = gather_from_widgets();
    cfg_.save(config_path());
    emit config_changed(cfg_);
}

}  // namespace choir
