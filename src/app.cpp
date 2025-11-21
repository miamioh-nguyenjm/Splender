// app.cpp
// it's the application...

#include "app.h"
#include "loader.h"
#include "ui.h"
#include "renderer.h"
#include "globals.h"
#include "usersettings.h"

#include "imgui.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <memory>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstddef>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellapi.h>
  #include <cstdint>
  #include <commdlg.h>

  #ifndef GLFW_EXPOSE_NATIVE_WIN32
  #define GLFW_EXPOSE_NATIVE_WIN32
  #endif
  #include <GLFW/glfw3native.h>
#endif

static std::vector<unsigned int> build_edge_list(const std::vector<unsigned int>& triIndices) {
    struct Edge { unsigned int a, b; };
    struct EdgeHash {
        size_t operator()(Edge const& e) const noexcept {
            return (static_cast<size_t>(e.a) << 32) ^ static_cast<size_t>(e.b);
        }
    };
    struct EdgeEq { bool operator()(Edge const& x, Edge const& y) const noexcept { return x.a == y.a && x.b == y.b; } };

    auto make_key = [](unsigned int i1, unsigned int i2) -> Edge {
        if (i1 < i2) return Edge{ i1, i2 };
        return Edge{ i2, i1 };
    };

    std::unordered_set<Edge, EdgeHash, EdgeEq> edges;
    edges.reserve(triIndices.size() / 2);

    for (size_t i = 0; i + 2 < triIndices.size(); i += 3) {
        unsigned int i0 = triIndices[i + 0];
        unsigned int i1 = triIndices[i + 1];
        unsigned int i2 = triIndices[i + 2];
        edges.insert(make_key(i0, i1));
        edges.insert(make_key(i1, i2));
        edges.insert(make_key(i2, i0));
    }

    std::vector<unsigned int> lineIdx;
    lineIdx.reserve(edges.size() * 2);
    for (const auto &e : edges) {
        lineIdx.push_back(e.a);
        lineIdx.push_back(e.b);
    }
    return lineIdx;
}

// -------------------- Impl (PIMPL styule) -------------
struct App::Impl {
    int argc;
    char** argv;
    GLFWwindow* window = nullptr;

    Renderer renderer;

    // model GPU handles
    GLuint model_vao = 0;
    GLuint model_vbo = 0;
    GLuint model_ebo = 0;
    size_t model_index_count = 0;
    size_t currentVertexCount = 0;


    // line overlay handles (explicit edge
    GLuint model_lines_ebo = 0;
    size_t model_lines_count = 0;

    // lighting & view state (owned by app)
    glm::vec3 lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f));
    float lightIntensity = 1.0f;
    glm::vec3 lightColor = glm::vec3(1.0f);
    bool staticShadows = false;

    // camera/orbit
    double lastX = 0.0, lastY = 0.0; bool firstMouse = true;
    float yaw = glm::radians(-45.0f), pitch = glm::radians(25.0f);
    float distance = 6.0f;
    glm::vec3 target = glm::vec3(0.0f);
    CameraState camState;

    Loader loader;

    UserSettings userSettings;

    std::future<void> importLoaderFuture;
    std::shared_ptr<std::vector<glm::vec3>> import_positions_ptr;
    std::shared_ptr<std::vector<glm::vec3>> import_normals_ptr;
    std::shared_ptr<std::vector<unsigned int>> import_indices_ptr;
    std::shared_ptr<std::atomic<bool>> import_ready;
    std::shared_ptr<std::atomic<bool>> import_failed;
    std::shared_ptr<std::atomic<float>> import_progress;

    // state flags
    bool modelUploaded = false;
    bool showWireframe = false;
    bool prevEPressed = false;

    Impl(int a, char** v): argc(a), argv(v) {}
    ~Impl() {}

    bool initWindowAndGL() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, 16);
        window = glfwCreateWindow(1280, 720, "Splender 0.4.2", nullptr, nullptr);
        if (!window) { glfwTerminate(); return false; }

        std::string exeDir;
#if defined(_WIN32)
        // Try to load a Windows .ico file named "splender_logo.ico" in the exe directory.
        {
            std::filesystem::path iconPath = std::filesystem::path(exeDir) / "splender_logo.ico";
            std::string iconPathStr = iconPath.string();

            HICON hIconLarge = (HICON)LoadImageA(nullptr, iconPathStr.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
            if (hIconLarge) {
                HWND hwnd = glfwGetWin32Window(window);
                if (hwnd) {
                    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconLarge);
                    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconLarge);
                }
            } else {

            }
        }
#else
        // non-windows png
#endif

        // NEW: user settings file near exe
        std::filesystem::path settingsPath = std::filesystem::path(exeDir) / "usersettings.json";
        userSettings.filePath = settingsPath.string();
        userSettings.load(); // if missing, defaults remain

        glfwMaximizeWindow(window);
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);

        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { std::cerr << "glad init failed\n"; return false; }
        glEnable(GL_MULTISAMPLE);
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST);

        const char* glsl_version = "#version 330";
        if (!Ui_Init(window, glsl_version)) {
            std::cerr << "Ui_Init failed\n";
            return false;
        }

        // camera state
        camState.distance = &distance;
        camState.minDistance = 0.5f;
        camState.maxDistance = 100.0f;
        camState.zoomSpeed = 0.6f;
        glfwSetWindowUserPointer(window, &camState);
        glfwSetScrollCallback(window, [](GLFWwindow* w, double, double yoff){
            CameraState* s = static_cast<CameraState*>(glfwGetWindowUserPointer(w));
            if (!s || !s->distance) return;
            if (isLoading.load()) return;
            float d = *s->distance;
            d -= float(yoff) * s->zoomSpeed;
            d = std::clamp(d, s->minDistance, s->maxDistance);
            *s->distance = d;
        });

        return true;
    }

    bool compileBuiltinPrograms() {
        if (!renderer.createBuiltinPrograms()) return false;
        return true;
    }

    void startInitialLoad(const std::string& model_path) {
        isLoading.store(true);
        loader.startInitialLoad(model_path);
    }

    void requestImportAsync(const std::string& path) {
        loader.requestImportAsync(path);
        isLoading.store(true);
    }

    // Called each frame on main thread to swap in import when ready
    void maybeFinishImport() {
        // First prefer the Loader-managed completion path
        if (loader.maybeFinishImport()) {
            // delete old GL buffers
            if (model_ebo) { glDeleteBuffers(1, &model_ebo); model_ebo = 0; }
            if (model_vbo) { glDeleteBuffers(1, &model_vbo); model_vbo = 0; }
            if (model_vao) { glDeleteVertexArrays(1, &model_vao); model_vao = 0; }
            if (model_lines_ebo) { glDeleteBuffers(1, &model_lines_ebo); model_lines_ebo = 0; model_lines_count = 0; }

            modelUploaded = false;
            // loader will populate its positions()/normals()/indices(), upload happens in uploadModelIfReady()
            return;
        }

        if (import_ready && import_ready->load()) {
            // If import failed, log and clear state
            if (import_failed && import_failed->load()) {
                std::cerr << "Import (UI-initiated) failed to parse\n";
                // clear import state
                import_ready.reset(); import_failed.reset(); import_progress.reset();
                import_positions_ptr.reset(); import_normals_ptr.reset(); import_indices_ptr.reset();
                isLoading.store(false);
                return;
            }

            // Ensure we have imported buffers
            if (!import_positions_ptr || !import_normals_ptr || !import_indices_ptr) {
                std::cerr << "Import (UI-initiated) signalled ready but buffers are missing\n";
                import_ready.reset(); import_failed.reset(); import_progress.reset();
                import_positions_ptr.reset(); import_normals_ptr.reset(); import_indices_ptr.reset();
                isLoading.store(false);
                return;
            }

            // Delete old GL buffers
            if (model_ebo) { glDeleteBuffers(1, &model_ebo); model_ebo = 0; }
            if (model_vbo) { glDeleteBuffers(1, &model_vbo); model_vbo = 0; }
            if (model_vao) { glDeleteVertexArrays(1, &model_vao); model_vao = 0; }
            if (model_lines_ebo) { glDeleteBuffers(1, &model_lines_ebo); model_lines_ebo = 0; model_lines_count = 0; }

            // Build interleaved vertex array from import_ pointers
            std::vector<float> verts; verts.reserve(import_positions_ptr->size() * 6);
            for (size_t i = 0; i < import_positions_ptr->size(); ++i) {
                verts.push_back((*import_positions_ptr)[i].x);
                verts.push_back((*import_positions_ptr)[i].y);
                verts.push_back((*import_positions_ptr)[i].z);
                verts.push_back((*import_normals_ptr)[i].x);
                verts.push_back((*import_normals_ptr)[i].y);
                verts.push_back((*import_normals_ptr)[i].z);
            }

            // Create GL objects and upload
            glGenVertexArrays(1, &model_vao);
            glGenBuffers(1, &model_vbo);
            glGenBuffers(1, &model_ebo);

            glBindVertexArray(model_vao);
            glBindBuffer(GL_ARRAY_BUFFER, model_vbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, import_indices_ptr->size() * sizeof(unsigned int), import_indices_ptr->data(), GL_STATIC_DRAW);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
            glBindVertexArray(0);

            model_index_count = import_indices_ptr->size();
            currentVertexCount = import_positions_ptr->size();


            // build explicit line EBO for wireframe overlay
            std::vector<unsigned int> lineIndices = build_edge_list(*import_indices_ptr);
            if (!lineIndices.empty()) {
                glBindVertexArray(model_vao); // element array binds to VAO
                glGenBuffers(1, &model_lines_ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_lines_ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, lineIndices.size() * sizeof(unsigned int), lineIndices.data(), GL_STATIC_DRAW);
                model_lines_count = lineIndices.size();
                // restore triangle EBO as VAO element array
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_ebo);
                glBindVertexArray(0);
            } else {
                model_lines_count = 0;
            }

            modelUploaded = true;

            // Clear import state
            import_ready.reset(); import_failed.reset(); import_progress.reset();
            import_positions_ptr.reset(); import_normals_ptr.reset(); import_indices_ptr.reset();
            if (importLoaderFuture.valid()) importLoaderFuture = std::future<void>();

            // Mark loading finished
            isLoading.store(false);
        }
    }


    void uploadModelIfReady() {
        if (!modelUploaded && loader.modelReady.load()) {
            auto positions_ptr = loader.positions();
            loader.loadProgress.store(0.0f);
            auto normals_ptr = loader.normals();
            auto indices_ptr = loader.indices();
            if (!positions_ptr || !normals_ptr || !indices_ptr) return;

            std::vector<float> verts; verts.reserve(positions_ptr->size()*6);
            for (size_t i = 0; i < positions_ptr->size(); ++i) {
                verts.push_back((*positions_ptr)[i].x);
                verts.push_back((*positions_ptr)[i].y);
                verts.push_back((*positions_ptr)[i].z);
                verts.push_back((*normals_ptr)[i].x);
                verts.push_back((*normals_ptr)[i].y);
                verts.push_back((*normals_ptr)[i].z);
            }

            glGenVertexArrays(1, &model_vao);
            glGenBuffers(1, &model_vbo);
            glGenBuffers(1, &model_ebo);

            glBindVertexArray(model_vao);
            glBindBuffer(GL_ARRAY_BUFFER, model_vbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices_ptr->size()*sizeof(unsigned int), indices_ptr->data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));
            glBindVertexArray(0);

            model_index_count = indices_ptr->size();
            currentVertexCount = positions_ptr->size();

            if (model_lines_ebo) { glDeleteBuffers(1, &model_lines_ebo); model_lines_ebo = 0; model_lines_count = 0; }
            std::vector<unsigned int> lineIndices = build_edge_list(*indices_ptr);
            if (!lineIndices.empty()) {
                glBindVertexArray(model_vao);
                glGenBuffers(1, &model_lines_ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_lines_ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, lineIndices.size() * sizeof(unsigned int), lineIndices.data(), GL_STATIC_DRAW);
                model_lines_count = lineIndices.size();
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_ebo);
                glBindVertexArray(0);
            }

            modelUploaded = true;
            isLoading.store(false);
        }
    }

    void shutdownCleanup() {
        if (model_ebo) { glDeleteBuffers(1, &model_ebo); model_ebo = 0; }
        if (model_vbo) { glDeleteBuffers(1, &model_vbo); model_vbo = 0; }
        if (model_vao) { glDeleteVertexArrays(1, &model_vao); model_vao = 0; }
        if (model_lines_ebo) { glDeleteBuffers(1, &model_lines_ebo); model_lines_ebo = 0; model_lines_count = 0; }

        renderer.shutdownCleanup();
    }
};

// ---------- App public methods ------------

App::App(int argc, char** argv) : impl_(std::make_unique<Impl>(argc, argv)) {}

App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;

App::~App() {
    if (impl_) {
        if (impl_->window) {
            glfwMakeContextCurrent(impl_->window);
            Ui_Shutdown();
            impl_->shutdownCleanup();
            glfwDestroyWindow(impl_->window);
            impl_->window = nullptr;
            glfwTerminate();
        }
    }
}

int App::run() {
    Impl& I = *impl_;

    if (!I.initWindowAndGL()) return -1;
    if (!I.renderer.init()) return -1;
    if (!I.compileBuiltinPrograms()) return -1;

    std::string model_path = std::filesystem::path(std::filesystem::current_path() / "assets" / "splender.obj").string();
    if (I.argc > 1) model_path = std::string(I.argv[1]);
    std::cout << "Model path: " << model_path << "\n";

    I.startInitialLoad(model_path);

    // UI import refs wired to app loader state (if UI starts imports it places data here)
    ImportStateRefs importRefs;
    importRefs.importLoaderFuture = &I.importLoaderFuture;
    importRefs.import_positions_ptr = &I.import_positions_ptr;
    importRefs.import_normals_ptr = &I.import_normals_ptr;
    importRefs.import_indices_ptr = &I.import_indices_ptr;
    importRefs.import_ready = &I.import_ready;
    importRefs.import_failed = &I.import_failed;
    importRefs.import_progress = &I.import_progress;

    while (!glfwWindowShouldClose(I.window)) {
        // Input: cursor and mouse
        double mx, my; glfwGetCursorPos(I.window, &mx, &my);
        int middleState = glfwGetMouseButton(I.window, GLFW_MOUSE_BUTTON_MIDDLE);
        bool altState = (glfwGetKey(I.window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS) || (glfwGetKey(I.window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS);
        if (I.firstMouse) { I.lastX = mx; I.lastY = my; I.firstMouse = false; }

        // Keyboard toggle for wireframe (single-press)
        {
            ImGuiIO& io = ImGui::GetIO();
            if (!io.WantCaptureKeyboard) {
                int eState = glfwGetKey(I.window, GLFW_KEY_E);
                bool ePressed = (eState == GLFW_PRESS);
                if (ePressed && !I.prevEPressed) {
                    I.showWireframe = !I.showWireframe;
                }
                I.prevEPressed = ePressed;
            } else {
                I.prevEPressed = false;
            }
        }

        if (!isLoading.load()) {
            if (middleState == GLFW_PRESS) {
                double dx = mx - I.lastX, dy = my - I.lastY;

                bool doOrbit = false;
                bool doPan = false;

                if (I.userSettings.control == ControlScheme::Industry) {
                    // Industry: pan = middle, orbit = Alt + middle (3ds max, maya, houdini, etc.)
                    if (altState) doOrbit = true;
                    else doPan = true;
                } else { // Blender
                    bool shiftState = (glfwGetKey(I.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) || (glfwGetKey(I.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
                    if (shiftState) doPan = true;
                    else doOrbit = true;
                }

                if (doOrbit) {
                    I.yaw += float(dx) * 0.005f;
                    I.pitch += float(dy) * 0.005f;
                    const float pl = glm::radians(89.0f); I.pitch = glm::clamp(I.pitch, -pl, pl);
                } else if (doPan) {
                    float cy = cos(I.yaw), sy = sin(I.yaw);
                    float cp = cos(I.pitch), sp = sin(I.pitch);
                    glm::vec3 forward = glm::normalize(glm::vec3(cp * cy, sp, cp * sy));
                    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));
                    glm::vec3 up = glm::normalize(glm::cross(right, forward));
                    glm::vec3 panOffset = float(dx) * 0.005f * right + float(dy) * 0.005f * up;
                    I.target += panOffset * I.distance * 0.2f;
                }
            }
        }

        I.lastX = mx; I.lastY = my;

        int fbW, fbH; glfwGetFramebufferSize(I.window, &fbW, &fbH);

        // Draw background first (renderer)
        I.renderer.drawBackground();

        glClear(GL_DEPTH_BUFFER_BIT);

        // Camera matrices
        float cx = I.distance * cos(I.pitch) * cos(I.yaw);
        float cy = I.distance * sin(I.pitch);
        float cz = I.distance * cos(I.pitch) * sin(I.yaw);
        glm::vec3 camPos = I.target + glm::vec3(cx, cy, cz);
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), fbW>0 ? (float)fbW/fbH : 1.0f, 0.01f, 1000.0f);
        glm::mat4 view = glm::lookAt(camPos, I.target, glm::vec3(0,1,0));
        glm::mat4 model = glm::mat4(1.0f);
        glm::mat4 mvp = proj * view * model;

        // Handle imports and uploads
        I.maybeFinishImport();
        I.uploadModelIfReady();

        // Set renderer uniforms and draw model if ready
        if (I.renderer.modelProgram()) {
            glm::vec3 finalLightDir = I.lightDir;
            if (!I.staticShadows) finalLightDir = glm::normalize(camPos - I.target);

            I.renderer.setModelMVP(mvp);
            I.renderer.setModelMatrix(model);
            I.renderer.setLightDirection(finalLightDir);
            I.renderer.setLightIntensity(I.lightIntensity);
            I.renderer.setLightColor(I.lightColor);
            I.renderer.setEnableShadows(I.staticShadows);

            if (I.modelUploaded) {
                glUseProgram(I.renderer.modelProgram());
                glBindVertexArray(I.model_vao);
                glDrawElements(GL_TRIANGLES, (GLsizei)I.model_index_count, GL_UNSIGNED_INT, 0);
                glBindVertexArray(0);
                glUseProgram(0);
            }
        }

        // Wireframe overlay passes
        if (I.showWireframe && I.modelUploaded && I.model_lines_count > 0) {
            I.renderer.setModelMVP(mvp);
            I.renderer.setModelMatrix(model);
            I.renderer.setForceWire(true);
            I.renderer.setWireColor(glm::vec3(0.45f,0.83f,0.28f));

            glUseProgram(I.renderer.modelProgram());
            glBindVertexArray(I.model_vao);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, I.model_lines_ebo);

            glEnable(GL_DEPTH_TEST);
            glLineWidth(2.0f);
            glDrawElements(GL_LINES, (GLsizei)I.model_lines_count, GL_UNSIGNED_INT, 0);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, I.model_ebo);
            glBindVertexArray(0);
            glUseProgram(0);

            I.renderer.setForceWire(false);

            // second pass
            I.renderer.setModelMVP(mvp);
            I.renderer.setModelMatrix(model);

            glUseProgram(I.renderer.modelProgram());
            glBindVertexArray(I.model_vao);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, I.model_lines_ebo);

            glLineWidth(1.0f);
            glEnable(GL_DEPTH_TEST);
            glDrawElements(GL_LINES, (GLsizei)I.model_lines_count, GL_UNSIGNED_INT, 0);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, I.model_ebo);
            glBindVertexArray(0);
            glUseProgram(0);
        }

        // Grid
        if (I.renderer.gridProgram()) {
            glm::mat4 mvpGrid = proj * view * glm::mat4(1.0f);
            I.renderer.drawGrid(mvpGrid);
        }

        size_t vertexCount = I.currentVertexCount;
        size_t triCount = (I.model_index_count > 0) ? (I.model_index_count / 3) : 0;

        // pass App's import_progress so the UI reads the same progress the UI-import writes to
        std::shared_ptr<std::atomic<float>> uiImportProgress = I.import_progress ? I.import_progress : I.loader.currentImportProgress();

        Ui_FrameDraw(I.window,
                    I.loader.loadProgress,
                    uiImportProgress,
                    importRefs,
                    I.lightDir,
                    I.lightIntensity,
                    I.lightColor,
                    I.staticShadows,
                    &I.showWireframe,
                    I.userSettings,
                    vertexCount,
                    triCount);

        glfwSwapBuffers(I.window);
        glfwPollEvents();
    }

    // Wait for loader work to finish before shutdown (loader manages futures internally)
    Ui_Shutdown();
    I.shutdownCleanup();
    if (I.window) {
        glfwDestroyWindow(I.window);
        I.window = nullptr;
    }
    glfwTerminate();
    return 0;
}

void App::requestImport(const std::string& objPath) {
    if (!impl_) return;
    if (isLoading.load()) return;
    impl_->requestImportAsync(objPath);
}

void App::shutdown() {
    if (impl_ && impl_->window) glfwSetWindowShouldClose(impl_->window, GLFW_TRUE);
}
