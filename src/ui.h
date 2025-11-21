#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <future>
#include <vector>

#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

#include "globals.h"
#include "usersettings.h"

bool Ui_Init(GLFWwindow* window, const char* glsl_version = "#version 330");
void Ui_Shutdown();
void Ui_NewFrame();
void Ui_Render();

// Import state references used by UI to hand imported buffers back to the app
struct ImportStateRefs {
    std::future<void>* importLoaderFuture = nullptr;
    std::shared_ptr<std::vector<glm::vec3>>* import_positions_ptr = nullptr;
    std::shared_ptr<std::vector<glm::vec3>>* import_normals_ptr = nullptr;
    std::shared_ptr<std::vector<unsigned int>>* import_indices_ptr = nullptr;
    std::shared_ptr<std::atomic<bool>>* import_ready = nullptr;
    std::shared_ptr<std::atomic<bool>>* import_failed = nullptr;
    std::shared_ptr<std::atomic<float>>* import_progress = nullptr;
};

// Ui_FrameDraw: draw all UI elements for the frame, mutating app view state via references
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
                  size_t triCount);

bool Ui_WantsCaptureMouse();
bool Ui_WantsCaptureKeyboard();
