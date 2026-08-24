// BinPickerDialog.hpp — modal dialog to pick one or more _wow_ bins from
// the open BigFile (port of gui/bin_picker_dialog.py).
//
// Returns the selected bins' *internal* resource keys (the form expected
// by _wol_ deps and the rest of the resource graph) — not their BF FAT
// keys. The two are different namespaces; see [[jade-bigio-key-table]].
#pragma once

#include <QDialog>
#include <QThread>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace jade { class BigFile; }
class QLabel;
class QLineEdit;
class QTableWidget;

// Background-build the internal-key index (decompresses every wow's head —
// only ~64 bytes per file, but ~3000 files in SoT).
class BinIndexBuilder : public QThread {
    Q_OBJECT
public:
    explicit BinIndexBuilder(std::shared_ptr<jade::BigFile> bf,
                             QObject* parent = nullptr)
        : QThread(parent), bf_(std::move(bf)) {}

    std::map<quint32, quint32> take_index() { return std::move(index_); }

signals:
    void progress(int done, int total);
    void result(const QString& error);  // empty error == success

protected:
    void run() override;

private:
    std::shared_ptr<jade::BigFile> bf_;
    std::map<quint32, quint32> index_;  // internal_key -> BF file index
};

// Modal dialog: pick one or more _wow_ bins by name.
//
// Args:
//   bigfile: the open jade::BigFile.
//   title: dialog title (e.g. "Pick deps for new zone").
//   already_chosen: set of internal keys to mark as already in the
//       target's dep list (rendered greyed-out so the user doesn't add a
//       duplicate).
//
// Returns (via selected_keys()) a list of *internal* resource keys for
// the bins the user accepted.
class BinPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit BinPickerDialog(std::shared_ptr<jade::BigFile> bigfile,
                             const QString& title = QStringLiteral("Pick bins"),
                             const std::set<quint32>& already_chosen = {},
                             QWidget* parent = nullptr);

    const std::vector<quint32>& selected_keys() const {
        return selected_keys_;
    }

private slots:
    void on_progress(int done, int total);
    void on_done(const QString& err);
    void on_thread_done();
    void apply_filter();
    void on_accept();

private:
    void start_build();
    void populate();

    std::shared_ptr<jade::BigFile> bf_;
    std::set<quint32> already_;
    std::map<quint32, quint32> index_;  // internal_key -> BF file index
    bool have_index_ = false;
    std::vector<quint32> selected_keys_;
    BinIndexBuilder* builder_ = nullptr;

    QLineEdit* filter_ = nullptr;
    QLabel* status_ = nullptr;
    QTableWidget* table_ = nullptr;
};
