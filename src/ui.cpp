// ui.cpp
// Centralized ImGui UI s. Implements ui.h...

#include "ui.h"
#include "usersettings.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <memory>
#include <future>
#include <atomic>
#include <vector>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cmath>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <commdlg.h>
#endif

// Forward declaration of the loader function used by async tasks.
extern bool load_model_simple(const std::string& path,
                              std::vector<glm::vec3>& out_positions,
                              std::vector<glm::vec3>& out_normals,
                              std::vector<unsigned int>& out_indices,
                              std::atomic<float>* progress);

static bool g_uiInitialized = false;
static GLFWwindow* g_window = nullptr;

bool Ui_Init(GLFWwindow* window, const char* glsl_version)
{
    if (g_uiInitialized) return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Try to load custom font from assets/fonts/Inter_18pt-Regular.ttf relative to exe
    std::string exeDir;
#if defined(_WIN32)
    char exePathBuf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
    if (len != 0 && len < MAX_PATH) exeDir = std::filesystem::path(exePathBuf).parent_path().string();
    else exeDir = ".";
#else
    exeDir = std::filesystem::current_path().string();
#endif

    std::filesystem::path fontPath = std::filesystem::path(exeDir) / "assets" / "fonts" / "Inter_18pt-Regular.ttf";
    const std::string customFontPath = fontPath.string();
    const float customFontSize = 16.0f;

    if (std::ifstream(customFontPath).good()) {
        ImFont* f = io.Fonts->AddFontFromFileTTF(customFontPath.c_str(), customFontSize);
        if (f) io.FontDefault = f;
    }

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    g_uiInitialized = true;
    g_window = window;
    return true;
}

void Ui_Shutdown()
{
    if (!g_uiInitialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_uiInitialized = false;
    g_window = nullptr;
}

void Ui_NewFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Ui_Render()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool Ui_WantsCaptureMouse() { return ImGui::GetIO().WantCaptureMouse; }
bool Ui_WantsCaptureKeyboard() { return ImGui::GetIO().WantCaptureKeyboard; }

// Internal helpers ----------------------------------------------------------

static void draw_main_menu_bar(std::atomic<bool>& isLoading,
                              ImportStateRefs& importRefs,
                              bool* showWireframe,
                              UserSettings& userSettings)
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        ImGui::Separator();
        ImGui::BeginDisabled(isLoading.load());

#if defined(_WIN32)
        if (ImGui::BeginMenu("Import")) {
            auto do_open_and_start = [&](const char* filter) {
                char szFile[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = nullptr;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = (DWORD)sizeof(szFile);
                ofn.lpstrFilter = filter;
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameA(&ofn)) {
                    const std::string chosenPath = std::string(ofn.lpstrFile);

                    auto new_positions = std::make_shared<std::vector<glm::vec3>>();
                    auto new_normals   = std::make_shared<std::vector<glm::vec3>>();
                    auto new_indices   = std::make_shared<std::vector<unsigned int>>();
                    auto newModelReady = std::make_shared<std::atomic<bool>>(false);
                    auto newModelFailed= std::make_shared<std::atomic<bool>>(false);
                    auto newProgress   = std::make_shared<std::atomic<float>>(0.0f);

                    isLoading.store(true);

                    std::future<void> fut = std::async(std::launch::async,
                        [chosenPath, new_positions, new_normals, new_indices, newModelReady, newModelFailed, newProgress]() {
                            bool ok = load_model_simple(chosenPath, *new_positions, *new_normals, *new_indices, newProgress.get());
                            if (!ok) newModelFailed->store(true);
                            else newModelReady->store(true);
                        });

                    if (importRefs.importLoaderFuture) *(importRefs.importLoaderFuture) = std::move(fut);
                    if (importRefs.import_positions_ptr) *(importRefs.import_positions_ptr) = new_positions;
                    if (importRefs.import_normals_ptr)   *(importRefs.import_normals_ptr)   = new_normals;
                    if (importRefs.import_indices_ptr)   *(importRefs.import_indices_ptr)   = new_indices;
                    if (importRefs.import_ready)         *(importRefs.import_ready)         = newModelReady;
                    if (importRefs.import_failed)        *(importRefs.import_failed)        = newModelFailed;
                    if (importRefs.import_progress)      *(importRefs.import_progress)      = newProgress;
                }
            };

            if (ImGui::MenuItem("OBJ...")) {
                do_open_and_start("Wavefront OBJ (*.obj)\0*.obj;*.OBJ\0All files\0*.*\0");
            }
            if (ImGui::MenuItem("FBX...")) {
                do_open_and_start("Autodesk FBX (*.fbx)\0*.fbx;*.FBX\0All files\0*.*\0");
            }
            if (ImGui::MenuItem("glTF / GLB...")) {
                do_open_and_start("glTF Binary / JSON (*.glb;*.gltf)\0*.glb;*.gltf\0All files\0*.*\0");
            }
            if (ImGui::MenuItem("Collada / DAE...")) {
                do_open_and_start("Collada DAE (*.dae)\0*.dae;*.DAE\0All files\0*.*\0");
            }
            if (ImGui::MenuItem("PLY...")) {
                do_open_and_start("Stanford Triangle Format (*.ply)\0*.ply;*.PLY\0All files\0*.*\0");
            }
            if (ImGui::MenuItem("STL...")) {
                do_open_and_start("STL (Binary/ASCII) (*.stl)\0*.stl;*.STL\0All files\0*.*\0");
            }

            ImGui::EndMenu();
        }
#else
        if (ImGui::MenuItem("Import")) {
            ImGui::OpenPopup("Import Not Implemented");
        }
        if (ImGui::BeginPopupModal("Import Not Implemented", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Native open-file dialog for Import is only implemented on Windows in this build.");
            if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
#endif

        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    // View menu: provide Wireframe menu item (mutates app showWireframe via pointer)
    if (ImGui::BeginMenu("View")) {
        if (showWireframe) {
            bool current = *showWireframe;
            if (ImGui::MenuItem("Wireframe", "E", &current)) {
                *showWireframe = current;
            }
        } else {
            // If no pointer provided, still show disabled item for parity
            ImGui::BeginDisabled();
            ImGui::MenuItem("Wireframe", "E", false, false);
            ImGui::EndDisabled();
        }
        ImGui::EndMenu();
    }
    // Edit ->  Preferences
    static bool showPrefsWindow = false;
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Preferences...")) {
            showPrefsWindow = true;
        }
        ImGui::EndMenu();
    }

    // Preferences modal window (simple modeless window)
    if (showPrefsWindow) {
        ImGui::SetNextWindowSize(ImVec2(840,840), ImGuiCond_Appearing);
        if (ImGui::Begin("Preferences", &showPrefsWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("General");
            ImGui::Separator();
            ImGui::TextUnformatted("Control scheme");
            int cs = (userSettings.control == ControlScheme::Blender) ? 1 : 0;
            ImGui::RadioButton("Industry", &cs, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Blender", &cs, 1);
            if (cs == 0) userSettings.control = ControlScheme::Industry;
            else userSettings.control = ControlScheme::Blender;

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ImGui::Button("Save")) {
                userSettings.save();
                showPrefsWindow = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                // reload from disk to revert in-memory changes
                userSettings.load();
                showPrefsWindow = false;
            }
            ImGui::End();
        }
    }

    ImGui::EndMainMenuBar();
}

static void draw_view_controls_panel(glm::vec3& lightDir,
                                     float& lightIntensity,
                                     glm::vec3& lightColor,
                                     bool& staticShadows,
                                     size_t vertexCount,
                                     size_t triCount)
{
    const ImVec2 panelSize(340.0f, 300.0f);
    const ImVec2 margin(18.0f, 8.0f);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 workPos = vp->WorkPos;
    ImVec2 workSize = vp->WorkSize;
    ImVec2 panelPos(workPos.x + workSize.x - panelSize.x - margin.x,
                    workPos.y + margin.y);

    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(panelSize, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                        | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoSavedSettings
                        | ImGuiWindowFlags_NoFocusOnAppearing;

    if (!ImGui::Begin("ViewControls", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::SetWindowFontScale(1.12f);
    ImGui::TextUnformatted("View Controls");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();

    ImGui::Text("Vertices: %zu", vertexCount);
    ImGui::SameLine(); ImGui::Text("Triangles: %zu", triCount);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Separator();
    ImGui::TextUnformatted("Lighting");
    ImGui::Separator();

    float azimuth = atan2f(lightDir.z, lightDir.x);
    float elevation = asinf(glm::clamp(lightDir.y, -1.0f, 1.0f));
    float azDeg = glm::degrees(azimuth);
    float elDeg = glm::degrees(elevation);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float itemW = avail.x - 100.0f;
    if (itemW < 80.0f) itemW = 80.0f;

    ImGui::BeginDisabled(!staticShadows);
    ImGui::PushItemWidth(itemW);
    if (ImGui::SliderFloat("Azimuth", &azDeg, -180.0f, 180.0f)) {
        azimuth = glm::radians(azDeg);
    }
    if (ImGui::SliderFloat("Elevation", &elDeg, -89.0f, 89.0f)) {
        elevation = glm::radians(elDeg);
    }
    ImGui::PopItemWidth();
    ImGui::EndDisabled();

    if (staticShadows) {
        lightDir.x = cosf(elevation) * cosf(azimuth);
        lightDir.y = sinf(elevation);
        lightDir.z = cosf(elevation) * sinf(azimuth);
        float len = std::sqrt(lightDir.x*lightDir.x + lightDir.y*lightDir.y + lightDir.z*lightDir.z);
        if (len > 1e-6f) {
            lightDir.x /= len;
            lightDir.y /= len;
            lightDir.z /= len;
        }
    }

    ImGui::SliderFloat("Intensity", &lightIntensity, 0.0f, 4.0f, "%.2f");

    float col[3] = { lightColor.r, lightColor.g, lightColor.b };
    if (ImGui::ColorEdit3("Light Color", col)) {
        lightColor = glm::vec3(col[0], col[1], col[2]);
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::Checkbox("Static Lighting", &staticShadows);

    if (!staticShadows) {
        ImGui::BeginDisabled();
        ImGui::TextDisabled("Headlamp light on");
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    ImGui::End();
}

static void draw_loading_modal(GLFWwindow* win,
                               const std::atomic<bool>& isLoading,
                               const std::shared_ptr<std::atomic<float>>& import_progress_ptr,
                               const std::atomic<float>& loadProgress)
{
    if (!isLoading.load()) return;

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(win, &fbW, &fbH);
    int boxW = static_cast<int>(fbW * 0.5f);
    ImVec2 winSize((float)boxW, 96.0f);
    ImVec2 winPos((fbW - boxW) * 0.5f, (fbH - (int)winSize.y) * 0.5f);

    ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoDecoration
                             | ImGuiWindowFlags_NoMove
                             | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoSavedSettings
                             | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGui::Begin("LoadingModal", nullptr, winFlags);
    ImGui::TextColored(ImVec4(0.9f,0.9f,0.9f,1.0f), "Loading model...");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    float frac = 0.0f;
    if (import_progress_ptr) frac = import_progress_ptr->load();
    else frac = loadProgress.load();
    frac = glm::clamp(frac, 0.0f, 1.0f);

    ImGui::ProgressBar(frac, ImVec2((float)boxW - 24.0f, 18.0f));
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::SameLine();
    ImGui::Text("%d%%", (int)std::round(frac * 100.0f));
    ImGui::End();
}

// Public composite frame draw ------------------------------------------------

void Ui_FrameDraw(GLFWwindow* win,
                  const std::atomic<float>& loadProgress,
                  const std::shared_ptr<std::atomic<float>>& import_progress_ptr,
                  ImportStateRefs& importRefs,
                  glm::vec3& lightDir,
                  float& lightIntensity,
                  glm::vec3& lightColor,
                  bool& staticShadows,
                  bool* showWireframe,
                  UserSettings& userSettings,
                  size_t vertexCount,
                  size_t triCount)
{
    if (!g_uiInitialized) return;

    Ui_NewFrame();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 ArdentOrange = ImVec4(1.00f, 0.72f, 0.00f, 1.0f);
    ImVec4 ArdentOrange_hover = ImVec4(1.00f, 0.82f, 0.00f, 1.0f);
    ImVec4 ArdentOrange_active = ImVec4(1.00f, 0.82f, 0.00f, 1.0f);
    style.Colors[ImGuiCol_Button]        = ArdentOrange;
    style.Colors[ImGuiCol_ButtonHovered] = ArdentOrange_hover;
    style.Colors[ImGuiCol_ButtonActive]  = ArdentOrange_active;
    style.Colors[ImGuiCol_FrameBg]       = ImVec4(0.10f,0.10f,0.10f,1.0f);
    style.Colors[ImGuiCol_FrameBgHovered]= ImVec4(0.13f,0.13f,0.13f,1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f,0.16f,0.16f,1.0f);
    style.Colors[ImGuiCol_SliderGrab]        = ArdentOrange;
    style.Colors[ImGuiCol_SliderGrabActive]  = ArdentOrange_active;
    style.Colors[ImGuiCol_Header]       = ImVec4(0.12f,0.12f,0.12f,1.0f);
    style.Colors[ImGuiCol_HeaderHovered]= ImVec4(0.22f,0.22f,0.22f,1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ArdentOrange;
    style.FrameRounding = 6.0f;
    style.GrabRounding  = 6.0f;
    style.WindowRounding = 6.0f;
    style.ItemSpacing = ImVec2(8,6);

    // Main menu bar this may start background imports via importRefs
    draw_main_menu_bar(isLoading, importRefs, showWireframe, userSettings);

    // Draw the view controls panel with app-supplied state so it appears and can mutate app state
    draw_view_controls_panel(lightDir, lightIntensity, lightColor, staticShadows, vertexCount, triCount);

    // Loading modal: uses import_progress_ptr when available, otherwise uses loadProgress
    draw_loading_modal(win, isLoading, import_progress_ptr, loadProgress);

    Ui_Render();
}
