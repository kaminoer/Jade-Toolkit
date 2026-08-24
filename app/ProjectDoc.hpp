// ProjectDoc.hpp — the ModProject document model (port of
// project/project.py + project/assets.py's AssetStore surface).
//
// Holds an ordered list of operation records (raw JSON dicts, matching the
// core builder's representation) and a reference to the base game archive
// (by filename + content hash, never an absolute path). Mutators record
// undo snapshots; a Qt `changed` signal lets the UI react.
//
// Design rule: this class knows *nothing* about how to actually build a
// .bf — that's jade::project::build_project. A ProjectDoc is pure data
// plus a small amount of behaviour (undo, asset import).
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <vector>

#include "jade/Json.hpp"
#include "jade/Project.hpp"

class ProjectDoc : public QObject {
    Q_OBJECT
public:
    static constexpr int MAX_UNDO_DEPTH = 100;

    // ---- construction / lifecycle ----

    explicit ProjectDoc(QObject* parent = nullptr);

    // Create a brand-new project targeting the archive at
    // base_archive_path. The path is hashed; no copy is made yet.
    // Returns nullptr + *err on a missing file.
    static ProjectDoc* create(const QString& name, const QString& game,
                              const QString& base_archive_path,
                              QObject* parent = nullptr,
                              QString* err = nullptr);

    // Load a project from a .jmod directory. Returns nullptr + *err
    // (the ProjectFormatError text) on failure.
    static ProjectDoc* load(const QString& jmod_dir,
                            QObject* parent = nullptr,
                            QString* err = nullptr);

    // Write the project to jmod_dir (or path()). Creates the directory and
    // assets/ + thumbnails/ if needed. Returns the directory path, or an
    // empty string with *err set.
    QString save(const QString& jmod_dir = QString(), QString* err = nullptr);

    // ---- plain data (mirroring the Python attributes) ----

    QString name, author, description, created, modified;
    jade::project::BaseRef base;
    jade::project::BuildSettings build;
    std::vector<jade::json::Value> operations;  // raw op dicts, in order
    QString path;  // .jmod dir; empty if unsaved

    // ---- serialisation ----

    jade::json::Value to_dict() const;

    // A jade::project::ModProject view of this document, for handing to the
    // core builder / validator (they take the core struct).
    jade::project::ModProject to_core() const;

    // ---- assets (AssetStore) ----

    QString assets_dir() const;  // requires a saved path
    // Copy a file into the asset store, return its "asset:<hash>" ref
    // (deduped by content hash). Empty + *err on failure.
    QString import_asset(const QString& src_path, QString* err = nullptr);
    // Store generated data; ext is appended verbatim, matching add_bytes().
    QString import_asset_bytes(const QByteArray& data,
                               const QString& ext = QString(),
                               QString* err = nullptr);
    // Absolute path for an "asset:<hash>" ref, or empty if absent.
    QString resolve_asset(const QString& ref) const;
    bool asset_exists(const QString& ref) const;
    QStringList all_asset_refs() const;
    int gc_assets(const QStringList& live_refs) const;

    // ---- operations ----

    // Append an operation dict, assigning it a stable "id" if missing.
    // Returns the assigned id.
    QString add_operation(jade::json::Value op);
    bool remove_operation(const QString& op_id);
    bool move_operation(const QString& op_id, int new_index);
    bool set_enabled(const QString& op_id, bool enabled);
    bool set_label(const QString& op_id, const QString& label);
    jade::json::Value* get_operation(const QString& op_id);
    std::vector<const jade::json::Value*> enabled_operations() const;

    // project.ModProject.conflicts(ctx). Current Python signatures do not
    // consume ctx, so the native document can expose the structured result
    // directly and share it with build validation.
    std::vector<jade::project::ProjectConflict> conflicts() const;

    // ---- undo / redo ----

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    bool undo();
    bool redo();

    // ---- dirty flag ----

    bool is_dirty() const { return dirty_; }
    // Mutations made directly on `operations` / metadata by callers that
    // bypass the mutator API must call touch() to keep dirty + UI in sync.
    void touch();
    void push_undo();

    // ISO-8601 UTC timestamp, Z-suffixed, seconds precision (iso_now()).
    static QString iso_now();

signals:
    void changed();

private:
    QString allocate_op_id();
    int index_of(const QString& op_id) const;
    std::vector<jade::json::Value> snapshot() const { return operations; }
    void restore(std::vector<jade::json::Value> snap) {
        operations = std::move(snap);
    }

    int next_op_serial_ = 1;
    bool dirty_ = false;
    std::vector<std::vector<jade::json::Value>> undo_stack_;
    std::vector<std::vector<jade::json::Value>> redo_stack_;
};
