#include "ProjectDoc.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "jade/ProjectAssets.hpp"
#include "jade/Sha256.hpp"

using jade::json::Value;
using jade::json::make_bool;
using jade::json::make_str;

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }
std::string s8(const QString& s) { return s.toStdString(); }

// serialize.py::_default_output_name — "<stem>.modded<ext>".
QString default_output_name(const QString& archive_name) {
    QFileInfo fi(archive_name);
    const QString stem = fi.completeBaseName();
    QString ext = fi.suffix();
    ext = ext.isEmpty() ? QStringLiteral(".bf") : "." + ext;
    return stem + ".modded" + ext;
}

QString op_id_of(const Value& op) {
    const Value* v = op.find("id");
    return v && v->is_str() ? q(v->str) : QString();
}

bool replace_file(const QString& source, const QString& target,
                  QString* error) {
#ifdef Q_OS_WIN
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                    reinterpret_cast<LPCWSTR>(target.utf16()),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    if (error)
        *error = QStringLiteral("native error %1").arg(GetLastError());
    return false;
#else
    const QByteArray source_name = QFile::encodeName(source);
    const QByteArray target_name = QFile::encodeName(target);
    if (std::rename(source_name.constData(), target_name.constData()) == 0)
        return true;
    if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
#endif
}

}  // namespace

ProjectDoc::ProjectDoc(QObject* parent) : QObject(parent) {
    created = iso_now();
    modified = created;
    build.strict_inplace = true;
}

QString ProjectDoc::iso_now() {
    return QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyy-MM-ddTHH:mm:ss'Z'"));
}

// ---- construction / lifecycle ----

ProjectDoc* ProjectDoc::create(const QString& name, const QString& game,
                               const QString& base_archive_path,
                               QObject* parent, QString* err) {
    QFileInfo fi(base_archive_path);
    if (!fi.isFile()) {
        if (err) *err = QStringLiteral("no such file: %1").arg(base_archive_path);
        return nullptr;
    }
    auto* proj = new ProjectDoc(parent);
    proj->name = name;
    proj->base.game = s8(game);
    proj->base.archive_name = s8(fi.fileName());
    proj->base.archive_sha256 = jade::sha256_file_hex(s8(base_archive_path));
    proj->base.archive_size = static_cast<uint64_t>(fi.size());
    proj->build.output_name = s8(default_output_name(fi.fileName()));
    proj->dirty_ = true;
    return proj;
}

ProjectDoc* ProjectDoc::load(const QString& jmod_dir, QObject* parent,
                             QString* err) {
    // The core loader owns format validation (serialize.py port).
    jade::project::ModProject core = jade::project::load_project(s8(jmod_dir));
    if (!core.ok) {
        if (err) *err = q(core.error);
        return nullptr;
    }
    auto* proj = new ProjectDoc(parent);
    proj->path = jmod_dir;
    proj->name = q(core.name);
    proj->author = q(core.author);
    proj->description = q(core.description);
    proj->created = core.created.empty() ? iso_now() : q(core.created);
    proj->modified = core.modified.empty() ? proj->created : q(core.modified);
    proj->base = core.base;
    proj->build = core.build;
    proj->next_op_serial_ = core.next_op_serial;
    proj->operations = std::move(core.operations);
    proj->dirty_ = false;
    return proj;
}

QString ProjectDoc::save(const QString& jmod_dir, QString* err) {
    QString target = jmod_dir.isEmpty() ? path : jmod_dir;
    if (target.isEmpty()) {
        if (err)
            *err = QStringLiteral(
                "save: no target directory; pass jmod_dir or set path");
        return QString();
    }
    target = QFileInfo(target).absoluteFilePath();
    QDir().mkpath(target);
    QDir().mkpath(target + "/assets");
    QDir().mkpath(target + "/thumbnails");
    modified = iso_now();

    // serialize.save_project_dict writes the fixed project.json.tmp and then
    // atomically replaces project.json. Keeping that exact temporary name is
    // observable when open/write/replacement fails.
    const std::string text = jade::json::dump(to_dict(), 2) + "\n";
    const QString temporary_path = target + "/project.json.tmp";
    const QString final_path = target + "/project.json";
    QFile f(temporary_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("failed to write %1: %2")
                       .arg(f.fileName(), f.errorString());
        return QString();
    }

    // Deterministic I/O seams used only by the parity oracle. Python's
    // json.dump ignores a short text-stream write result, so "short" must
    // still proceed to replacement with the truncated (empty) file.
    const QString fault = qEnvironmentVariable("JADE_PROJECTDOC_SAVE_FAULT");
    bool write_ok = true;
    if (fault == QStringLiteral("write")) {
        write_ok = false;
    } else if (fault != QStringLiteral("short")) {
        write_ok = f.write(text.data(), qint64(text.size())) ==
                   qint64(text.size());
    }
    if (!f.flush()) write_ok = false;
    const QString write_error = f.errorString();
    f.close();
    if (!write_ok) {
        if (err)
            *err = QStringLiteral("failed to write %1: %2")
                       .arg(temporary_path, write_error);
        return QString();
    }

    QString replace_error;
    if (!replace_file(temporary_path, final_path, &replace_error)) {
        if (err)
            *err = QStringLiteral("failed to write %1: %2")
                       .arg(final_path, replace_error);
        return QString();
    }
    path = target;
    dirty_ = false;
    return target;
}

// ---- serialisation ----

Value ProjectDoc::to_dict() const {
    return jade::project::project_to_dict(to_core());
}

jade::project::ModProject ProjectDoc::to_core() const {
    jade::project::ModProject core;
    core.ok = true;
    core.name = s8(name);
    core.author = s8(author);
    core.description = s8(description);
    core.created = s8(created);
    core.modified = s8(modified);
    core.base = base;
    core.build = build;
    core.jmod_dir = s8(path);
    core.next_op_serial = next_op_serial_;
    core.operations = operations;
    return core;
}

// ---- assets (AssetStore) ----

QString ProjectDoc::assets_dir() const {
    return path.isEmpty() ? QString() : path + "/assets";
}

QString ProjectDoc::import_asset(const QString& src_path, QString* err) {
    if (path.isEmpty()) {
        if (err) *err = QStringLiteral("project has no path yet; save first");
        return QString();
    }
    try {
        jade::project_assets::AssetStore store(s8(assets_dir()));
        return q(store.add(s8(src_path)));
    } catch (const std::exception& e) {
        if (err) *err = q(e.what());
        return QString();
    }
}

QString ProjectDoc::import_asset_bytes(const QByteArray& data,
                                       const QString& ext, QString* err) {
    if (path.isEmpty()) {
        if (err) *err = QStringLiteral("project has no path yet; save first");
        return QString();
    }
    try {
        jade::project_assets::AssetStore store(s8(assets_dir()));
        const auto* begin = reinterpret_cast<const uint8_t*>(data.constData());
        return q(store.add_bytes(
            std::vector<uint8_t>(begin, begin + data.size()), s8(ext)));
    } catch (const std::exception& e) {
        if (err) *err = q(e.what());
        return QString();
    }
}

QString ProjectDoc::resolve_asset(const QString& ref) const {
    if (path.isEmpty()) return QString();
    try {
        return q(jade::project_assets::AssetStore(s8(assets_dir())).resolve(
            s8(ref)));
    } catch (const std::exception&) {
        return QString();
    }
}

bool ProjectDoc::asset_exists(const QString& ref) const {
    if (path.isEmpty()) return false;
    try {
        return jade::project_assets::AssetStore(s8(assets_dir())).exists(
            s8(ref));
    } catch (const std::exception&) {
        return false;
    }
}

QStringList ProjectDoc::all_asset_refs() const {
    QStringList refs;
    if (path.isEmpty()) return refs;
    try {
        for (const std::string& ref :
             jade::project_assets::AssetStore(s8(assets_dir())).all_refs())
            refs.push_back(q(ref));
    } catch (const std::exception&) {
    }
    return refs;
}

int ProjectDoc::gc_assets(const QStringList& live_refs) const {
    if (path.isEmpty()) return 0;
    std::vector<std::string> refs;
    refs.reserve(size_t(live_refs.size()));
    for (const QString& ref : live_refs) refs.push_back(s8(ref));
    try {
        return int(jade::project_assets::AssetStore(s8(assets_dir())).gc(refs));
    } catch (const std::exception&) {
        return 0;
    }
}

// ---- operations ----

QString ProjectDoc::add_operation(Value op) {
    push_undo();
    op = jade::project::operation_to_dict(op);
    QString id = op_id_of(op);
    if (id.isEmpty()) {
        id = allocate_op_id();
        op.obj["id"] = make_str(s8(id));
    }
    if (!op.has("created")) op.obj["created"] = make_str(s8(iso_now()));
    if (!op.has("enabled")) op.obj["enabled"] = make_bool(true);
    if (!op.has("label")) op.obj["label"] = make_str("");
    operations.push_back(std::move(op));
    touch();
    return id;
}

bool ProjectDoc::remove_operation(const QString& op_id) {
    const int idx = index_of(op_id);
    if (idx < 0) return false;
    push_undo();
    operations.erase(operations.begin() + idx);
    touch();
    return true;
}

bool ProjectDoc::move_operation(const QString& op_id, int new_index) {
    const int idx = index_of(op_id);
    if (idx < 0) return false;
    new_index = qBound(0, new_index, int(operations.size()) - 1);
    if (new_index == idx) return false;
    push_undo();
    Value op = std::move(operations[idx]);
    operations.erase(operations.begin() + idx);
    operations.insert(operations.begin() + new_index, std::move(op));
    touch();
    return true;
}

bool ProjectDoc::set_enabled(const QString& op_id, bool enabled) {
    Value* op = get_operation(op_id);
    if (!op) return false;
    const Value* cur = op->find("enabled");
    const bool cur_enabled = !cur || cur->type != Value::Type::Bool || cur->b;
    if (cur_enabled == enabled) return false;
    push_undo();
    op->obj["enabled"] = make_bool(enabled);
    touch();
    return true;
}

bool ProjectDoc::set_label(const QString& op_id, const QString& label) {
    Value* op = get_operation(op_id);
    if (!op) return false;
    const Value* cur = op->find("label");
    if (cur && cur->is_str() && q(cur->str) == label) return false;
    push_undo();
    op->obj["label"] = make_str(s8(label));
    touch();
    return true;
}

Value* ProjectDoc::get_operation(const QString& op_id) {
    const int idx = index_of(op_id);
    return idx >= 0 ? &operations[size_t(idx)] : nullptr;
}

std::vector<const Value*> ProjectDoc::enabled_operations() const {
    std::vector<const Value*> out;
    for (const Value& op : operations) {
        const Value* en = op.find("enabled");
        if (!en || en->type != Value::Type::Bool || en->b) out.push_back(&op);
    }
    return out;
}

std::vector<jade::project::ProjectConflict> ProjectDoc::conflicts() const {
    return jade::project::project_conflicts(to_core());
}

// ---- undo / redo ----

bool ProjectDoc::undo() {
    if (undo_stack_.empty()) return false;
    redo_stack_.push_back(snapshot());
    restore(std::move(undo_stack_.back()));
    undo_stack_.pop_back();
    dirty_ = true;
    emit changed();
    return true;
}

bool ProjectDoc::redo() {
    if (redo_stack_.empty()) return false;
    undo_stack_.push_back(snapshot());
    restore(std::move(redo_stack_.back()));
    redo_stack_.pop_back();
    dirty_ = true;
    emit changed();
    return true;
}

// ---- internals ----

QString ProjectDoc::allocate_op_id() {
    return QStringLiteral("op-%1").arg(next_op_serial_++, 4, 10,
                                       QLatin1Char('0'));
}

int ProjectDoc::index_of(const QString& op_id) const {
    for (size_t i = 0; i < operations.size(); ++i)
        if (op_id_of(operations[i]) == op_id) return int(i);
    return -1;
}

void ProjectDoc::push_undo() {
    undo_stack_.push_back(snapshot());
    if (int(undo_stack_.size()) > MAX_UNDO_DEPTH)
        undo_stack_.erase(undo_stack_.begin());
    redo_stack_.clear();
}

void ProjectDoc::touch() {
    dirty_ = true;
    emit changed();
}
