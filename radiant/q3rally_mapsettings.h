/* Q3Rally map setup and worldspawn editor. GPLv2 or later. */
#pragma once
#include <QDialog>
#include <QMainWindow>
#include <QDialogButtonBox>
#include <QTabWidget>
#include <QScrollArea>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QMessageBox>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <cmath>
#include "ientity.h"

using Q3RallyMapValues = QMap<QString, QString>;
struct Q3RallyWorldspawnField {
    const char *group, *key, *defaultValue, *type, *description;
};
static const Q3RallyWorldspawnField q3rallyWorldspawnFields[] = {
#include "q3rally_mapsettings_fields.inc"
};
class Q3RallyWorldspawnVisitor : public Entity::Visitor {
    Q3RallyMapValues &values;
public:
    explicit Q3RallyWorldspawnVisitor(Q3RallyMapValues &v) : values(v) {}
    void visit(const char *key, const char *value) override {
        if (QString::fromUtf8(key) != "classname")
            values.insert(QString::fromUtf8(key), QString::fromUtf8(value));
    }
};
static Q3RallyMapValues Q3Rally_ReadWorldspawn(Entity *entity) {
    Q3RallyMapValues values;
    if (entity) { Q3RallyWorldspawnVisitor visitor(values); entity->forEachKeyValue(visitor); }
    return values;
}
static bool Q3Rally_ValidWorldspawnValue(const Q3RallyWorldspawnField &field, const QString &value) {
    const QString type = field.type;
    if (type == "text") return true;
    if (type == "weather") return value == "none" || value == "rain" || value == "snow" || value == "rain snow";
    if (type == "bool") return value == "0" || value == "1";
    const QStringList parts = value.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const bool pair = type == "cycle" || type == "strength" || type == "wind" || type == "pair";
    if (parts.size() < 1 || parts.size() > (pair ? 2 : 1)) return false;
    double previous = 0;
    for (int i = 0; i < parts.size(); ++i) {
        bool ok;
        double number = parts[i].toDouble(&ok);
        if (!ok || !std::isfinite(number)) return false;
        if (i && number < previous) return false;
        if (type == "unit" && (number < 0 || number > 1)) return false;
        if (type == "positive" && number <= 0) return false;
        if (type == "chance" && (number < 0 || number > 100 || std::floor(number) != number)) return false;
        if (type == "cycle" && number < 10) return false;
        if (type == "strength" && (number < 0.05 || number > 1)) return false;
        if (type == "wind" && (number < 0 || number > 600)) return false;
        if (type == "direction" && number < 0 && number != -1) return false;
        previous = number;
    }
    return true;
}

static bool Q3Rally_EditMapSettings(QWidget *parent, Entity *entity, bool newMap, Q3RallyMapValues &result) {
    const Q3RallyMapValues original = Q3Rally_ReadWorldspawn(entity);
    QDialog dialog(parent);
    dialog.setWindowTitle(newMap ? "New Q3Rally map - Worldspawn settings" : "Q3Rally Map Settings");
    dialog.resize(820, 640);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel("Choose the map's worldspawn settings. Checked keys are written explicitly. "
        "Unchecked keys use engine/compiler defaults. Hover over a key for help.");
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *tabs = new QTabWidget; layout->addWidget(tabs);
    QMap<QString, QFormLayout *> groups;
    struct Row { const Q3RallyWorldspawnField *field; QCheckBox *enabled; QLineEdit *value; QComboBox *choice; };
    QVector<Row> rows;
    QSet<QString> known;
    for (const auto &field : q3rallyWorldspawnFields) {
        const QString key = field.key, group = field.group;
        known.insert(key);
        if (!groups.contains(group)) {
            auto *scroll = new QScrollArea; scroll->setWidgetResizable(true);
            auto *page = new QWidget;
            auto *form = new QFormLayout(page);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            scroll->setWidget(page); tabs->addTab(scroll, group); groups.insert(group, form);
        }
        auto *enabled = new QCheckBox(key);
        auto *editor = new QWidget;
        auto *line = new QHBoxLayout(editor); line->setContentsMargins(0,0,0,0);
        auto *value = new QLineEdit;
        QComboBox *choice = nullptr;
        const QString type = field.type;
        if (type == "bool" || type == "weather") {
            choice = new QComboBox; choice->setEditable(true);
            if (type == "bool") choice->addItems({"0", "1"});
            else choice->addItems({"none", "rain", "snow", "rain snow"});
            line->addWidget(choice);
            value->hide(); value->setParent(editor);
        } else line->addWidget(value);
        const QString initial = original.contains(key) ? original.value(key) : QString(field.defaultValue);
        value->setText(initial); value->setPlaceholderText("Default / not set");
        if (choice) choice->setEditText(initial);
        enabled->setChecked(original.contains(key) || (newMap && !initial.isEmpty() && group != "Compiler and terrain"));
        editor->setEnabled(enabled->isChecked());
        QObject::connect(enabled, &QCheckBox::toggled, editor, &QWidget::setEnabled);
        const QString help = QString(field.description) + "\nKey: " + key + "\nDefault: " +
            (QString(field.defaultValue).isEmpty() ? "not set / engine or compiler default" : field.defaultValue);
        enabled->setToolTip(help); editor->setToolTip(help);
        groups[group]->addRow(enabled, editor);
        rows.push_back({&field, enabled, value, choice});
    }
    auto *extraPage = new QWidget;
    auto *extraLayout = new QVBoxLayout(extraPage);
    auto *extra = new QTableWidget(0, 2);
    extra->setHorizontalHeaderLabels({"Key", "Value"});
    extra->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    extraLayout->addWidget(new QLabel("Additional or custom worldspawn keys are preserved here."));
    extraLayout->addWidget(extra);
    for (auto it = original.cbegin(); it != original.cend(); ++it) if (!known.contains(it.key())) {
        int row = extra->rowCount(); extra->insertRow(row);
        extra->setItem(row, 0, new QTableWidgetItem(it.key()));
        extra->setItem(row, 1, new QTableWidgetItem(it.value()));
    }
    auto *extraActions = new QHBoxLayout;
    auto *add = new QPushButton("Add key"), *remove = new QPushButton("Remove selected key");
    extraActions->addWidget(add); extraActions->addWidget(remove); extraActions->addStretch(); extraLayout->addLayout(extraActions);
    QObject::connect(add, &QPushButton::clicked, [&]() { int row = extra->rowCount(); extra->insertRow(row); extra->setItem(row,0,new QTableWidgetItem); extra->setItem(row,1,new QTableWidgetItem); extra->setCurrentCell(row,0); extra->editItem(extra->item(row,0)); });
    QObject::connect(remove, &QPushButton::clicked, [&]() { if (extra->currentRow() >= 0) extra->removeRow(extra->currentRow()); });
    tabs->addTab(extraPage, "Additional keys");
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, [&]() {
        Q3RallyMapValues candidate;
        for (const auto &row : rows) if (row.enabled->isChecked()) {
            const QString key = row.field->key;
            const QString value = row.choice ? row.choice->currentText() : row.value->text();
            // Do not normalize or reject unchanged values in existing maps.
            if (newMap || !original.contains(key) || value != original.value(key)) {
                if (value.trimmed().isEmpty() || !Q3Rally_ValidWorldspawnValue(*row.field, value)) {
                    QMessageBox::warning(&dialog, "Invalid map setting", "Check the value for " + key + ".\n" + row.field->description);
                    return;
                }
            }
            candidate.insert(key,value);
        }
        for (int i=0; i<extra->rowCount(); ++i) {
            const QString key = extra->item(i,0) ? extra->item(i,0)->text().trimmed() : QString();
            const QString value = extra->item(i,1) ? extra->item(i,1)->text() : QString();
            if (key.isEmpty() && value.isEmpty()) continue;
            if (key.isEmpty() || key == "classname" || known.contains(key) || candidate.contains(key) ||
                key.contains(QRegularExpression("[\\s\"{}]")) || value.isEmpty()) {
                QMessageBox::warning(&dialog, "Invalid custom key", "Additional keys must have unique names and non-empty values; classname cannot be changed."); return;
            }
            candidate.insert(key,value);
        }
        bool trackAllowed = false, trackChanged = newMap;
        for (const char *key : {"allowShortTrack", "allowMediumTrack", "allowLongTrack"}) {
            trackAllowed |= candidate.value(key,"1").toInt() != 0;
            trackChanged |= candidate.value(key,"1") != original.value(key,"1");
        }
        if (trackChanged && !trackAllowed) { QMessageBox::warning(&dialog, "No race route", "Enable at least one track length."); return; }
        result = candidate; dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}
