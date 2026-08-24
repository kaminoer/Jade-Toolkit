#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#include "ProjectDoc.hpp"
#include "jade/Json.hpp"

using jade::json::Value;

namespace {

Value document_state(const char* tag, const ProjectDoc& project) {
    Value state = jade::json::make_obj();
    state.obj["tag"] = jade::json::make_str(tag);
    state.obj["project"] = project.to_dict();
    state.obj["dirty"] = jade::json::make_bool(project.is_dirty());
    state.obj["can_undo"] = jade::json::make_bool(project.can_undo());
    state.obj["can_redo"] = jade::json::make_bool(project.can_redo());
    return state;
}

Value new_stub(uint32_t entry_key, uint32_t sub_key) {
    Value operation = jade::json::make_obj();
    operation.obj["op"] = jade::json::make_str("stub_mesh");
    operation.obj["created"] =
        jade::json::make_str("2026-01-01T00:00:00Z");
    Value target = jade::json::make_obj();
    target.obj["entry_key"] = jade::json::make_num(entry_key);
    target.obj["sub_key"] = jade::json::make_num(sub_key);
    operation.obj["target"] = std::move(target);
    operation.obj["params"] = jade::json::make_obj();
    return operation;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 4 && std::strcmp(argv[1], "--save") == 0) {
        QString error;
        ProjectDoc* project = ProjectDoc::load(
            QString::fromLocal8Bit(argv[2]), nullptr, &error);
        if (!project) {
            std::fprintf(stderr, "load failed: %s\n",
                         error.toUtf8().constData());
            return 1;
        }
        const bool changed = project->set_label(
            QStringLiteral("op-0001"), QStringLiteral("saved-label"));
        const QString saved = project->save(
            QString::fromLocal8Bit(argv[3]), &error);
        Value output = jade::json::make_obj();
        output.obj["changed"] = jade::json::make_bool(changed);
        output.obj["ok"] = jade::json::make_bool(!saved.isEmpty());
        output.obj["error"] = jade::json::make_bool(!error.isEmpty());
        output.obj["result"] = jade::json::make_str(saved.toStdString());
        output.obj["path"] =
            jade::json::make_str(project->path.toStdString());
        output.obj["dirty"] = jade::json::make_bool(project->is_dirty());
        output.obj["modified"] =
            jade::json::make_str(project->modified.toStdString());
        std::printf("SAVE %s\n", jade::json::dump(output).c_str());
        delete project;
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: projectdoc_cli <jmod_dir> | --save <source> <target>\n");
        return 2;
    }
    QString error;
    ProjectDoc* project = ProjectDoc::load(
        QString::fromLocal8Bit(argv[1]), nullptr, &error);
    if (!project) {
        std::fprintf(stderr, "load failed: %s\n",
                     error.toUtf8().constData());
        return 1;
    }

    Value output = jade::json::make_obj();
    Value states = jade::json::make_arr();
    states.arr.push_back(document_state("initial", *project));

    Value noops = jade::json::make_obj();
    noops.obj["remove_missing"] = jade::json::make_bool(
        project->remove_operation(QStringLiteral("missing")));
    noops.obj["move_same"] = jade::json::make_bool(
        project->move_operation(QStringLiteral("op-0001"), 0));
    noops.obj["enable_same"] = jade::json::make_bool(
        project->set_enabled(QStringLiteral("op-0001"), true));
    noops.obj["label_same"] = jade::json::make_bool(
        project->set_label(QStringLiteral("op-0002"),
                           QStringLiteral("second")));
    output.obj["noops"] = std::move(noops);

    output.obj["first_added_id"] = jade::json::make_str(
        project->add_operation(new_stub(0x22000001, 0x4001)).toStdString());
    states.arr.push_back(document_state("after_add", *project));
    project->set_enabled(QStringLiteral("op-0001"), false);
    project->set_label(QStringLiteral("op-0002"), QStringLiteral("renamed"));
    project->move_operation(QStringLiteral("op-0004"), 0);
    project->remove_operation(QStringLiteral("op-0003"));
    states.arr.push_back(document_state("mutated", *project));

    int undo_count = 0;
    while (project->undo()) {
        ++undo_count;
        const std::string tag = "undo" + std::to_string(undo_count);
        states.arr.push_back(document_state(tag.c_str(), *project));
    }
    output.obj["undo_count"] = jade::json::make_num(undo_count);
    output.obj["extra_undo"] = jade::json::make_bool(project->undo());

    int redo_count = 0;
    while (project->redo()) {
        ++redo_count;
        const std::string tag = "redo" + std::to_string(redo_count);
        states.arr.push_back(document_state(tag.c_str(), *project));
    }
    output.obj["redo_count"] = jade::json::make_num(redo_count);
    output.obj["extra_redo"] = jade::json::make_bool(project->redo());

    project->undo();
    output.obj["branch_added_id"] = jade::json::make_str(
        project->add_operation(new_stub(0x22000001, 0x4002)).toStdString());
    states.arr.push_back(document_state("branched", *project));

    for (int index = 0; index < 105; ++index) {
        const QString label = QStringLiteral("depth-%1").arg(
            index, 3, 10, QLatin1Char('0'));
        project->set_label(QStringLiteral("op-0001"), label);
    }
    int depth_undo_count = 0;
    while (project->undo()) ++depth_undo_count;
    output.obj["depth_undo_count"] =
        jade::json::make_num(depth_undo_count);
    states.arr.push_back(document_state("depth_rewound", *project));
    output.obj["states"] = std::move(states);

    std::printf("DOCUMENT %s\n", jade::json::dump(output).c_str());
    delete project;
    return 0;
}
