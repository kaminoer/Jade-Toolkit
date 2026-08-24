// vtk_spike — the D1 viewport spike (GUI_REDESIGN.md).
//
//   vtk_spike <bf> <zone_name_substring> [--no-tex]
//
// Loads a zone through the NATIVE pipeline (discover_zones →
// levelblend::export_level_to_glb → our own glTF reader) and renders it
// in a plain VTK window. Measures the exit criteria:
//   * unlit 2003 look: LightingOff + base texture
//   * baked RLI: COLOR_0 as point scalars, texture MODULATE-blended
//   * perf: rolling FPS in the window title + P50/P95 on exit
//   * picking: left-click = vtkCellPicker, timing printed
//   * camera: trackball orbit (default), 'f' = fly (JoystickCamera)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkRendererCollection.h>
#include <vtkCellArray.h>
#include <vtkCellPicker.h>
#include <vtkFloatArray.h>
#include <vtkInteractorStyleJoystickCamera.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkPNGReader.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkTexture.h>
#include <vtkUnsignedCharArray.h>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Gltf.hpp"
#include "jade/Json.hpp"
#include "jade/LevelBlender.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Zone.hpp"

using namespace jade;

namespace {

std::vector<double> frame_times;

void on_render_end(vtkObject* caller, unsigned long, void*, void*) {
    auto* rw = static_cast<vtkRenderWindow*>(caller);
    double t = rw->GetRenderers()->GetFirstRenderer()
                   ->GetLastRenderTimeInSeconds();
    if (t > 0) {
        frame_times.push_back(t);
        char title[128];
        std::snprintf(title, sizeof title,
                      "vtk_spike — %.1f fps (last), %zu frames", 1.0 / t,
                      frame_times.size());
        rw->SetWindowName(title);
    }
}

void on_click(vtkObject* caller, unsigned long, void* client, void*) {
    auto* interactor = static_cast<vtkRenderWindowInteractor*>(caller);
    auto* renderer = static_cast<vtkRenderer*>(client);
    int x, y;
    interactor->GetEventPosition(x, y);
    vtkNew<vtkCellPicker> picker;
    picker->SetTolerance(0.0005);
    auto t0 = std::chrono::steady_clock::now();
    picker->Pick(x, y, 0, renderer);
    auto t1 = std::chrono::steady_clock::now();
    double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    vtkActor* a = picker->GetActor();
    std::printf("PICK %.2f ms  actor=%p cell=%lld\n", ms,
                static_cast<void*>(a),
                static_cast<long long>(picker->GetCellId()));
}

struct KeyState {
    vtkRenderWindowInteractor* interactor = nullptr;
    bool fly = false;
};

void on_key(vtkObject*, unsigned long, void* client, void*) {
    auto* ks = static_cast<KeyState*>(client);
    std::string key = ks->interactor->GetKeySym() != nullptr
                          ? ks->interactor->GetKeySym() : "";
    if (key == "f" || key == "F") {
        ks->fly = !ks->fly;
        if (ks->fly) {
            vtkNew<vtkInteractorStyleJoystickCamera> st;
            ks->interactor->SetInteractorStyle(st);
            std::printf("camera: fly (joystick)\n");
        } else {
            vtkNew<vtkInteractorStyleTrackballCamera> st;
            ks->interactor->SetInteractorStyle(st);
            std::printf("camera: orbit (trackball)\n");
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: vtk_spike <bf> <zone_substring> [--no-tex]\n");
        return 2;
    }
    bool use_tex = true;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--no-tex") use_tex = false;

    BigFile bf;
    bf.open(argv[1]);
    std::vector<ZoneInfo> zones = discover_zones(bf);
    const ZoneInfo* zone = nullptr;
    for (const ZoneInfo& z : zones)
        if (z.name.find(argv[2]) != std::string::npos) {
            zone = &z;
            break;
        }
    if (zone == nullptr) {
        std::fprintf(stderr, "no zone matching '%s' (%zu zones)\n", argv[2],
                     zones.size());
        return 1;
    }
    std::printf("zone: %s\n", zone->name.c_str());
    LzoResult dec = decompress_lzo(bf.read_data(zone->wow_index));
    if (!dec.ok) {
        std::fprintf(stderr, "zone did not decompress\n");
        return 1;
    }
    std::vector<SubEntry> subs = walk_sub_entries(dec.data);

    // Native zone → GLB (cooked meshes + COLOR_0 RLI + PNG textures).
    const std::string glb_path = "_spike_zone.glb";
    auto t0 = std::chrono::steady_clock::now();
    auto man = levelblend::export_level_to_glb(subs, glb_path,
                                               /*include_lights=*/false,
                                               &bf);
    auto t1 = std::chrono::steady_clock::now();
    if (!man.ok) {
        std::fprintf(stderr, "export failed: %s\n", man.error.c_str());
        return 1;
    }
    std::printf("export: %u objects in %.0f ms\n", man.objects,
                std::chrono::duration<double, std::milli>(t1 - t0).count());

    std::vector<uint8_t> glb;
    {
        FILE* f = std::fopen(glb_path.c_str(), "rb");
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        glb.resize(size_t(n));
        if (std::fread(glb.data(), 1, size_t(n), f) != size_t(n)) {
            std::fclose(f);
            return 1;
        }
        std::fclose(f);
    }
    gltf::GlbDoc doc = gltf::parse_glb(glb.data(), glb.size());
    const json::Value& g = doc.gltf;

    vtkNew<vtkRenderer> renderer;
    renderer->SetBackground(0.05, 0.08, 0.07);   // phthalo-ish

    // Textures: decode each PNG image once.
    std::vector<vtkSmartPointer<vtkTexture>> textures;
    const json::Value* images = g.find("images");
    const json::Value* bvs = g.find("bufferViews");
    if (use_tex && images != nullptr && images->is_arr()) {
        for (const json::Value& im : images->arr) {
            const json::Value* bvi = im.find("bufferView");
            vtkSmartPointer<vtkTexture> tex;
            if (bvi != nullptr && bvs != nullptr) {
                const json::Value& bv = bvs->arr[size_t(bvi->num)];
                size_t off = 0, len = 0;
                if (const json::Value* o = bv.find("byteOffset"))
                    off = size_t(o->num);
                if (const json::Value* l = bv.find("byteLength"))
                    len = size_t(l->num);
                vtkNew<vtkPNGReader> rd;
                rd->SetMemoryBuffer(doc.bin.data() + off);
                rd->SetMemoryBufferLength(long(len));
                rd->Update();
                tex = vtkSmartPointer<vtkTexture>::New();
                tex->SetInputConnection(rd->GetOutputPort());
                tex->InterpolateOn();
                tex->SetWrap(vtkTexture::Repeat);
                // MODULATE: texture × point-scalar colors = diffuse × RLI.
                tex->SetBlendingMode(
                    vtkTexture::VTK_TEXTURE_BLENDING_MODE_MODULATE);
            }
            textures.push_back(tex);
        }
    }
    auto tex_of_material = [&](long long mat_idx) -> vtkTexture* {
        if (mat_idx < 0) return nullptr;
        const json::Value* mats = g.find("materials");
        if (mats == nullptr || size_t(mat_idx) >= mats->arr.size())
            return nullptr;
        const json::Value* pbr =
            mats->arr[size_t(mat_idx)].find("pbrMetallicRoughness");
        const json::Value* bct =
            pbr != nullptr ? pbr->find("baseColorTexture") : nullptr;
        const json::Value* ti = bct != nullptr ? bct->find("index") : nullptr;
        if (ti == nullptr) return nullptr;
        const json::Value* texs = g.find("textures");
        if (texs == nullptr || size_t(ti->num) >= texs->arr.size())
            return nullptr;
        const json::Value* src = texs->arr[size_t(ti->num)].find("source");
        if (src == nullptr || size_t(src->num) >= textures.size())
            return nullptr;
        return textures[size_t(src->num)];
    };

    // World transform per mesh (row-major 16) from the scene graph.
    std::map<int, std::array<double, 16>> world =
        gltf::mesh_world_matrices(g);

    size_t n_actors = 0, n_tris = 0, n_colored = 0;
    const json::Value* meshes = g.find("meshes");
    if (meshes != nullptr && meshes->is_arr())
        for (size_t mi = 0; mi < meshes->arr.size(); ++mi) {
            std::array<double, 16> M = {1, 0, 0, 0, 0, 1, 0, 0,
                                        0, 0, 1, 0, 0, 0, 0, 1};
            auto wit = world.find(int(mi));
            if (wit != world.end()) M = wit->second;
            const json::Value* prims = meshes->arr[mi].find("primitives");
            if (prims == nullptr || !prims->is_arr()) continue;
            for (const json::Value& prim : prims->arr) {
                const json::Value* attrs = prim.find("attributes");
                const json::Value* pos =
                    attrs != nullptr ? attrs->find("POSITION") : nullptr;
                const json::Value* idx = prim.find("indices");
                if (pos == nullptr || idx == nullptr) continue;

                gltf::AccessorData pa =
                    gltf::read_accessor(doc, int(pos->num));
                vtkNew<vtkPoints> points;
                points->SetDataTypeToFloat();
                points->SetNumberOfPoints(pa.count);
                const float* pf =
                    reinterpret_cast<const float*>(pa.raw.data());
                for (vtkIdType i = 0; i < vtkIdType(pa.count); ++i) {
                    double x = pf[i * 3], y = pf[i * 3 + 1],
                           z = pf[i * 3 + 2];
                    points->SetPoint(
                        i, M[0] * x + M[1] * y + M[2] * z + M[3],
                        M[4] * x + M[5] * y + M[6] * z + M[7],
                        M[8] * x + M[9] * y + M[10] * z + M[11]);
                }

                gltf::AccessorData ia =
                    gltf::read_accessor(doc, int(idx->num));
                vtkNew<vtkCellArray> tris;
                const uint8_t* ip = ia.raw.data();
                for (size_t t = 0; t + 2 < ia.count; t += 3) {
                    vtkIdType ids[3];
                    for (int k = 0; k < 3; ++k) {
                        size_t j = t + size_t(k);
                        ids[k] = ia.comp_type == gltf::COMP_U32
                                     ? vtkIdType(reinterpret_cast<
                                                 const uint32_t*>(ip)[j])
                                     : vtkIdType(reinterpret_cast<
                                                 const uint16_t*>(ip)[j]);
                    }
                    tris->InsertNextCell(3, ids);
                }
                n_tris += ia.count / 3;

                vtkNew<vtkPolyData> poly;
                poly->SetPoints(points);
                poly->SetPolys(tris);

                const json::Value* uv =
                    attrs != nullptr ? attrs->find("TEXCOORD_0") : nullptr;
                if (uv != nullptr) {
                    gltf::AccessorData ua =
                        gltf::read_accessor(doc, int(uv->num));
                    vtkNew<vtkFloatArray> tc;
                    tc->SetNumberOfComponents(2);
                    tc->SetName("uv");
                    tc->SetNumberOfTuples(vtkIdType(ua.count));
                    const float* uf =
                        reinterpret_cast<const float*>(ua.raw.data());
                    for (vtkIdType i = 0; i < vtkIdType(ua.count); ++i)
                        tc->SetTuple2(i, uf[i * 2], uf[i * 2 + 1]);
                    poly->GetPointData()->SetTCoords(tc);
                }
                const json::Value* col =
                    attrs != nullptr ? attrs->find("COLOR_0") : nullptr;
                if (col != nullptr) {
                    auto rgba = gltf::colors_to_rgba255(
                        gltf::read_accessor(doc, int(col->num)));
                    vtkNew<vtkUnsignedCharArray> colors;
                    colors->SetNumberOfComponents(3);
                    colors->SetName("rli");
                    colors->SetNumberOfTuples(vtkIdType(rgba.size()));
                    for (vtkIdType i = 0; i < vtkIdType(rgba.size()); ++i)
                        colors->SetTuple3(
                            i, rgba[size_t(i)][0], rgba[size_t(i)][1],
                            rgba[size_t(i)][2]);
                    poly->GetPointData()->SetScalars(colors);
                    ++n_colored;
                }

                vtkNew<vtkPolyDataMapper> mapper;
                mapper->SetInputData(poly);
                mapper->SetColorModeToDirectScalars();
                vtkNew<vtkActor> actor;
                actor->SetMapper(mapper);
                actor->GetProperty()->LightingOff();   // unlit 2003 look
                const json::Value* mat = prim.find("material");
                vtkTexture* tex =
                    tex_of_material(mat != nullptr ? (long long)mat->num
                                                   : -1);
                if (tex != nullptr) actor->SetTexture(tex);
                renderer->AddActor(actor);
                ++n_actors;
            }
        }
    std::printf("scene: %zu actors, %zu tris, %zu colored primitives\n",
                n_actors, n_tris, n_colored);

    vtkNew<vtkRenderWindow> window;
    window->AddRenderer(renderer);
    window->SetSize(1280, 800);
    vtkNew<vtkRenderWindowInteractor> interactor;
    interactor->SetRenderWindow(window);
    vtkNew<vtkInteractorStyleTrackballCamera> style;
    interactor->SetInteractorStyle(style);

    vtkNew<vtkCallbackCommand> renderCb;
    renderCb->SetCallback(on_render_end);
    window->AddObserver(vtkCommand::EndEvent, renderCb);
    vtkNew<vtkCallbackCommand> clickCb;
    clickCb->SetCallback(on_click);
    clickCb->SetClientData(renderer);
    interactor->AddObserver(vtkCommand::LeftButtonPressEvent, clickCb);
    KeyState ks;
    ks.interactor = interactor;
    vtkNew<vtkCallbackCommand> keyCb;
    keyCb->SetCallback(on_key);
    keyCb->SetClientData(&ks);
    interactor->AddObserver(vtkCommand::KeyPressEvent, keyCb);

    bool bench = false;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--bench") bench = true;

    renderer->ResetCamera();
    window->Render();
    if (bench) {
        // Automated orbit: 240 frames of camera azimuth + a pick each 60.
        vtkCamera* cam = renderer->GetActiveCamera();
        auto b0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 240; ++i) {
            cam->Azimuth(1.5);
            renderer->ResetCameraClippingRange();
            window->Render();
            if (i % 60 == 30) {
                vtkNew<vtkCellPicker> picker;
                picker->SetTolerance(0.0005);
                auto p0 = std::chrono::steady_clock::now();
                picker->Pick(640, 400, 0, renderer);
                auto p1 = std::chrono::steady_clock::now();
                std::printf(
                    "PICK %.2f ms cell=%lld\n",
                    std::chrono::duration<double, std::milli>(p1 - p0)
                        .count(),
                    static_cast<long long>(picker->GetCellId()));
            }
        }
        auto b1 = std::chrono::steady_clock::now();
        double wall =
            std::chrono::duration<double>(b1 - b0).count();
        std::printf("BENCH 240 frames in %.2f s = %.1f fps (wall)\n", wall,
                    240.0 / wall);
    } else {
        interactor->Start();
    }

    if (!frame_times.empty()) {
        std::sort(frame_times.begin(), frame_times.end());
        double p50 = frame_times[frame_times.size() / 2];
        double p95 = frame_times[size_t(double(frame_times.size()) * 0.95)];
        std::printf("FPS p50=%.1f p95(slowest)=%.1f over %zu frames\n",
                    1.0 / p50, 1.0 / p95, frame_times.size());
    }
    return 0;
}
