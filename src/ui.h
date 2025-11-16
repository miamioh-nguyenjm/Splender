#pragma once

// ui.h

#include <string>
#include <functional>
#include <memory>
#include <atomic>

#include <glm/glm.hpp>

#include "globals.h" // provides extern std::atomic<bool> isLoading;


#include <GLFW/glfw3.h>

bool Ui_Init(GLFWwindow* window, const char* glsl_version = "#version 330");
void Ui_Shutdown();
void Ui_NewFrame();
void Ui_Render();



struct ImportRequest {
    std::string path;
    // progress is optional: worker can update a shared atomic<float>
    std::shared_ptr<std::atomic<float>> progress;
};

class UI {
public:
    // Callback signature to request an import from UI (runs on main thread).
    // Implementer typically starts an async worker that fills model buffers.
    using ImportCallback = std::function<void(const ImportRequest&)>;

    UI();
    ~UI();

    // Initialize ImGui context / backend. Must be called on the main thread after GL context is current.
    bool init(void* nativeWindowHandle, const char* glsl_version = "#version 330");

    // Shutdown ImGui and release UI resources.
    void shutdown();

    // Frame lifecycle: call at beginning of frame (new ImGui frame) and at end (render).
    void newFrame();
    void render();

    // Build the app main menu bar. Non-blocking. Supply ImportCallback to run when user chooses a file.
    // The implementation should disable menu actions while isLoading->load() == true.
    void buildMainMenu(const ImportCallback& importCb);

    // Show a centered modal loading overlay with a progress bar.
    // - progress: 0.0..1.0. If <0 then show indeterminate/spinner text.
    void showLoadingModal(float progress);

    // Helper to render a small status bar area (frame-rate, model name, etc.)
    void drawStatusBar(const std::string& statusText);

    // Returns whether ImGui is capturing mouse/keyboard (useful to short-circuit app input)
    bool wantsCaptureMouse() const;
    bool wantsCaptureKeyboard() const;

    // Non-copyable
    UI(const UI&) = delete;
    UI& operator=(const UI&) = delete;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

