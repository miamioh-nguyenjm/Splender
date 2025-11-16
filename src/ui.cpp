// ui.cpp
// UI module for Splender: initializes ImGui backends and draws main menu + loading modal.
// Meant to be compiled together with the rest of the project (links against ImGui, GLFW, OpenGL).
//
// Public functions:
//   bool Ui_Init(GLFWwindow* window, const char* glsl_version = "#version 330");
//   void Ui_Shutdown();
//   void Ui_NewFrame();
//   void Ui_Render();
//   void Ui_FrameDraw(GLFWwindow* win, std::atomic<bool>& isLoading, std::shared_ptr<std::atomic<float>> import_progress_ptr, const std::atomic<float>& loadProgress, ImportStateRefs& importRefs);
//
// Notes:
// - This file centralizes UI-only code and mirrors ImGui usage in the main app.
// - On Windows the native OpenFileDialog is used (GetOpenFileNameA). On other platforms the Import menu
//   item is shown but does nothing (keeps parity with the rest of the project).

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"


#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <memory>
#include <future>
#include <atomic>
#include <vector>
#include <glm/glm.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>

#include "ui.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <commdlg.h>
#endif

// Forward-declare the loader function signature used by main program so UI can start background imports.
// The actual loader is implemented in the core program (loader.cpp).
// bool load_obj_simple(const std::string& path, std::vector<glm::vec3>& out_positions, std::vector<glm::vec3>& out_normals, std::vector<unsigned int>& out_indices, std::atomic<float>* progress);

static bool g_uiInitialized = false;

// A convenience struct that groups the import state objects the main program keeps.
// Pass the main's variables by reference into DrawMainMenuBar so UI can start imports and update shared state.
struct ImportStateRefs {
    std::future<void>* importLoaderFuture; // pointer to main's future (may be nullptr)
    std::shared_ptr<std::vector<glm::vec3>>* import_positions_ptr;
    std::shared_ptr<std::vector<glm::vec3>>* import_normals_ptr;
    std::shared_ptr<std::vector<unsigned int>>* import_indices_ptr;
    std::shared_ptr<std::atomic<bool>>* import_ready;
    std::shared_ptr<std::atomic<bool>>* import_failed;
    std::shared_ptr<std::atomic<float>>* import_progress;
};

bool Ui_Init(GLFWwindow* window, const char* glsl_version)
{
    if (g_uiInitialized) return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // optional, keep existing flags

    // Resolve executable directory
    std::string exeDir;
#if defined(_WIN32)
    char exePathBuf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
    if (len != 0 && len < MAX_PATH) {
        std::filesystem::path p(exePathBuf);
        exeDir = p.parent_path().string();
    } else exeDir = ".";
#else
    exeDir = std::filesystem::current_path().string();
#endif

    // Build absolute font path and try to load it
    std::filesystem::path fontPath = std::filesystem::path(exeDir) / "assets" / "fonts" / "ShareTech-Regular.ttf";
    std::string customFontPath = fontPath.string();
    float customFontSize = 16.0f; // tweak as needed

    if (std::ifstream(customFontPath).good()) {
        ImFont* f = io.Fonts->AddFontFromFileTTF(customFontPath.c_str(), customFontSize);
        if (f) {
            io.FontDefault = f;
            std::cout << "ImGui: attempting font path: " << customFontPath << "  CWD: " << std::filesystem::current_path() << std::endl;
            std::cout << "ImGui: loaded custom font: " << customFontPath << std::endl;
        } else {
            std::cerr << "ImGui: AddFontFromFileTTF returned null for " << customFontPath << " - using default font\n";
        }
    } else {
        std::cerr << "ImGui: font file not found: " << customFontPath << " - using default font\n";
    }

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    g_uiInitialized = true;
    return true;
}

void Ui_Shutdown()
{
    if (!g_uiInitialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_uiInitialized = false;
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

// Draws the top main menu bar and starts an async import when the user picks a file.
// Parameters:
//   isLoading  - atomic<bool> used to disable menu while a load/import is active (shared with main).
//   importRefs - struct with pointers to main's import state (so UI can populate them).
// Note: This function will set isLoading.store(true) immediately when launching the async job.
void DrawMainMenuBar(std::atomic<bool>& isLoading, ImportStateRefs& importRefs)
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        // Disable the import menu while a load is in progress
        ImGui::BeginDisabled(isLoading.load());
        if (ImGui::MenuItem("Import")) {
#if defined(_WIN32)
            // Native Win32 open-file dialog (blocking UI momentarily while OS dialog open)
            char szFile[MAX_PATH] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = nullptr;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = (DWORD)sizeof(szFile);
            ofn.lpstrFilter = "Wavefront OBJ\0*.obj;*.OBJ\0All files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                std::string chosenPath = std::string(ofn.lpstrFile);

                // prepare fresh containers and flags
                auto new_positions = std::make_shared<std::vector<glm::vec3>>();
                auto new_normals   = std::make_shared<std::vector<glm::vec3>>();
                auto new_indices   = std::make_shared<std::vector<unsigned int>>();
                auto newModelReady = std::make_shared<std::atomic<bool>>(false);
                auto newModelFailed= std::make_shared<std::atomic<bool>>(false);
                auto newProgress   = std::make_shared<std::atomic<float>>(0.0f);

                // mark loading and launch async parser. The actual parser symbol must be available in the linking unit.
                isLoading.store(true);

                // Launch the loader asynchronously. The worker must call isLoading.store(false) when done.
                // We capture only the chosenPath and the shared containers so UI thread can set pointers into main's state.
                std::future<void> fut = std::async(std::launch::async,
                    [chosenPath, new_positions, new_normals, new_indices, newModelReady, newModelFailed, newProgress]() {
                        extern bool load_obj_simple(const std::string& path,
                                                   std::vector<glm::vec3>& out_positions,
                                                   std::vector<glm::vec3>& out_normals,
                                                   std::vector<unsigned int>& out_indices,
                                                   std::atomic<float>* progress);
                        bool ok = load_obj_simple(chosenPath, *new_positions, *new_normals, *new_indices, newProgress.get());
                        if (!ok) newModelFailed->store(true);
                        else newModelReady->store(true);
                        // Important: main program also owns a global isLoading and will set it false when appropriate.
                    });

                // store pointers into caller's importRefs so the main loop can see them
                if (importRefs.importLoaderFuture) {
                    *(importRefs.importLoaderFuture) = std::move(fut);
                }
                if (importRefs.import_positions_ptr) *(importRefs.import_positions_ptr) = new_positions;
                if (importRefs.import_normals_ptr)   *(importRefs.import_normals_ptr)   = new_normals;
                if (importRefs.import_indices_ptr)   *(importRefs.import_indices_ptr)   = new_indices;
                if (importRefs.import_ready)         *(importRefs.import_ready)         = newModelReady;
                if (importRefs.import_failed)        *(importRefs.import_failed)        = newModelFailed;
                if (importRefs.import_progress)      *(importRefs.import_progress)      = newProgress;
            }
#else
            // Non-Windows: Import not implemented 
#endif
        }
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// Draws an ImGui-centered loading modal based on provided progress atomics.
// If import_progress_ptr != nullptr, it will read progress from there; otherwise it reads loadProgress.
void DrawLoadingModal(GLFWwindow* win,
                      const std::atomic<bool>& isLoading,
                      const std::shared_ptr<std::atomic<float>>& import_progress_ptr,
                      const std::atomic<float>& loadProgress)
{
    if (!isLoading.load()) return;

    // Compose modal position/size based on framebuffer
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

// Small example wrapper that the main loop can call each frame to draw all UI bits.
// It expects main program to keep these variables and pass references to them so UI can read/write state.
void Ui_FrameDraw(GLFWwindow* win,
                  std::atomic<bool>& isLoading,
                  std::shared_ptr<std::atomic<float>> import_progress_ptr,
                  const std::atomic<float>& loadProgress,
                  ImportStateRefs& importRefs)
{
    Ui_NewFrame();

    // main menu bar (may kick off imports and mutate importRefs)
    DrawMainMenuBar(isLoading, importRefs);

    // draw loading modal on top if needed (reads progress)
    DrawLoadingModal(win, isLoading, import_progress_ptr, loadProgress);

    // render ImGui draw lists to GL
    Ui_Render();
}
