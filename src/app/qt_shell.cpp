#include "qt_shell.hpp"

#include "crescent.hpp"
#include "http_server.hpp"
#include "log.hpp"

#include "gekko_yomi/registry.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <thread>
#include <vector>

namespace gekko::yomi {
namespace {

QPixmap crescent_pixmap(int logical, qreal dpr) {
    QPixmap pixmap(qRound(logical * dpr), qRound(logical * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double pad = logical * 0.08;
    const double span = logical - 2.0 * pad;
    const double arm = std::max(1.15, logical * 0.055);
    const double ink_width = std::max(1.15, logical * 0.045);
    QPen edge(QColor(42, 26, 8));
    edge.setWidthF(ink_width + std::max(0.8, logical * 0.02));
    edge.setCapStyle(Qt::RoundCap);
    QPen ink(QColor(255, 224, 160));
    ink.setWidthF(ink_width);
    ink.setCapStyle(Qt::RoundCap);
    for (const CrescentPlus& mark : crescent_plus_marks()) {
        const double x = pad + mark.x * span;
        const double y = pad + mark.y * span;
        const QPointF horizontal_left(x - arm, y);
        const QPointF horizontal_right(x + arm, y);
        const QPointF vertical_top(x, y - arm);
        const QPointF vertical_bottom(x, y + arm);
        painter.setPen(edge);
        painter.drawLine(horizontal_left, horizontal_right);
        painter.drawLine(vertical_top, vertical_bottom);
        painter.setPen(ink);
        painter.drawLine(horizontal_left, horizontal_right);
        painter.drawLine(vertical_top, vertical_bottom);
    }
    return pixmap;
}

QIcon crescent_icon() {
    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    QIcon icon;
    for (const int size : {16, 22, 32, 48, 64}) {
        icon.addPixmap(crescent_pixmap(size, dpr));
    }
    return icon;
}

class EngineBox final : public QCheckBox {
public:
    explicit EngineBox(const QString& id, QWidget* parent) : QCheckBox(id, parent), id_(id) {}
    QString id() const { return id_; }

private:
    QString id_;
};

void edit_settings(QWidget* parent, HttpServer* server, Registry* registry, const std::string& config_path,
                   CoreConfig* config, const QIcon& icon) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Gekko Yomi settings");
    dialog.setWindowIcon(icon);
    dialog.setMinimumWidth(380);

    auto* ttl = new QSpinBox(&dialog);
    ttl->setRange(1, 1000000);
    ttl->setValue(config->buffer.ttl_scans);
    auto* positional = new QSpinBox(&dialog);
    positional->setRange(1, 1000000);
    positional->setValue(config->buffer.positional_invalidation_scans);

    auto* form = new QFormLayout;
    form->addRow("Vote lifetime (scans)", ttl);
    form->addRow("Empty scans before a box drops", positional);

    auto* all = new QCheckBox("Use every loaded engine", &dialog);
    all->setChecked(!config->ocr_engines.has_value());

    std::vector<std::string> ids = registry->registered_ocr_ids();
    if (config->ocr_engines) {
        for (const auto& id : *config->ocr_engines) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }
    auto* engines = new QGroupBox("OCR engines", &dialog);
    auto* engine_layout = new QVBoxLayout(engines);
    std::vector<EngineBox*> boxes;
    boxes.reserve(ids.size());
    if (ids.empty()) {
        engine_layout->addWidget(new QLabel("No OCR plugin is loaded.", engines));
    }
    for (const auto& id : ids) {
        auto* box = new EngineBox(QString::fromStdString(id), engines);
        const bool checked = !config->ocr_engines.has_value() ||
                             std::find(config->ocr_engines->begin(), config->ocr_engines->end(), id) !=
                                 config->ocr_engines->end();
        box->setChecked(checked);
        box->setEnabled(!all->isChecked());
        engine_layout->addWidget(box);
        boxes.push_back(box);
    }
    QObject::connect(all, &QCheckBox::toggled, &dialog, [boxes](bool every) {
        for (EngineBox* box : boxes) {
            box->setEnabled(!every);
        }
    });

    auto* note = new QLabel(
        "Save writes this file and applies it between scans. Turning an engine off stops its worker now. "
        "Turning one on starts it on the next scan.",
        &dialog);
    note->setWordWrap(true);
    if (config_path.empty()) {
        note->setText("HOME is unset, so there is no config file to write.");
    } else {
        note->setToolTip(QString::fromStdString(config_path));
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setEnabled(!config_path.empty());
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        CoreConfig next = *config;
        next.buffer.ttl_scans = ttl->value();
        next.buffer.positional_invalidation_scans = positional->value();
        if (all->isChecked()) {
            next.ocr_engines.reset();
        } else {
            std::vector<std::string> chosen;
            for (EngineBox* box : boxes) {
                if (box->isChecked()) {
                    chosen.push_back(box->id().toStdString());
                }
            }
            next.ocr_engines = std::move(chosen);
        }
        const std::string error = save_core_config(config_path, next);
        if (!error.empty()) {
            QMessageBox::warning(&dialog, "Gekko Yomi settings", QString::fromStdString(error));
            return;
        }
        server->request_config(next);
        *config = std::move(next);
        dialog.accept();
    });

    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(all);
    layout->addWidget(engines);
    layout->addWidget(note);
    if (!config_path.empty()) {
        auto* path = new QLabel(QString::fromStdString(config_path), &dialog);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(path);
    }
    layout->addWidget(buttons);
    dialog.exec();
}

}  // namespace

int run_qt_shell(int argc, char** argv, HttpServer* server, Registry* registry, const std::string& config_path,
                 CoreConfig config, const std::string& page_url) {
    int qt_argc = 1;
    char* qt_argv[] = {argc > 0 ? argv[0] : const_cast<char*>("gekko-yomi"), nullptr};
    QApplication app(qt_argc, qt_argv);
    app.setApplicationName("Gekko Yomi");
    app.setQuitOnLastWindowClosed(false);
    const QIcon icon = crescent_icon();
    app.setWindowIcon(icon);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        logger("app").warn("no system tray on this session; the page stays up without an icon");
        server->run();
        return 0;
    }

    std::thread server_thread([server] { server->run(); });

    const QUrl page(QString::fromStdString(page_url));
    auto open_page = [page] { QDesktopServices::openUrl(page); };

    QMenu menu;
    QAction* open = menu.addAction("Open page");
    QAction* settings = menu.addAction("Settings");
    menu.addSeparator();
    QAction* quit = menu.addAction("Quit");
    QObject::connect(open, &QAction::triggered, &app, open_page);
    QObject::connect(settings, &QAction::triggered, &app, [&] {
        edit_settings(nullptr, server, registry, config_path, &config, icon);
    });
    QObject::connect(quit, &QAction::triggered, &app, &QApplication::quit);

    QSystemTrayIcon tray(icon);
    tray.setToolTip("Gekko Yomi");
    tray.setContextMenu(&menu);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &app, [&](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            open_page();
        }
    });
    tray.show();
    logger("app").info("tray icon is up");

    const int code = app.exec();
    server->stop();
    server_thread.join();
    return code;
}

}  // namespace gekko::yomi
