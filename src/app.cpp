// app.cpp
// Refactored Splender-style application implementing App (app.h).
// - Moves main loop and GL/ImGui setup into App::Impl
// - Provides App::requestImport(...) to start an async import from UI callbacks
// - Ensures all GL work (uploads/draw) happens on main thread
// Build: link with glfw, glad, opengl, imgui backends. Requires stb_image, glm.

#include "app.h"
#include "loader.h"
#include "ui.h"

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
#include <algorithm>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>



#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

// ---------- simple shader helpers ----------
static GLuint compile_shader(GLenum t, const char* src){
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len=0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0'); glGetShaderInfoLog(s, len, nullptr, &log[0]);
        std::cerr << "Shader compile error:\n" << log << "\n"; glDeleteShader(s); return 0;
    }
    return s;
}
static GLuint link_program(GLuint vs, GLuint fs){
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len=0; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0'); glGetProgramInfoLog(p, len, nullptr, &log[0]);
        std::cerr << "Program link error:\n" << log << "\n"; glDeleteProgram(p); return 0;
    }
    return p;
}

// Minimal shaders
static const char* vs_src = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
void main(){ vNormal = mat3(transpose(inverse(uModel)))*aNormal; gl_Position = uMVP * vec4(aPos,1.0); }
)GLSL";

static const char* fs_src = R"GLSL(
#version 330 core
in vec3 vNormal; out vec4 fragColor;
void main(){ vec3 L = normalize(vec3(1.0,1.0,0.5)); float d = max(dot(normalize(vNormal),L),0.0); vec3 b = vec3(0.8); fragColor = vec4(b*d + b*0.15,1.0); }
)GLSL";

static const char* bg_vs_src = R"GLSL(
#version 330 core
out vec2 vUV;
void main(){ const vec2 V[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3)); vec2 p = V[gl_VertexID]; vUV = p*0.5+0.5; gl_Position = vec4(p,0,1); }
)GLSL";
static const char* bg_fs_src = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform vec3 uBottomGray; uniform vec3 uTopBlack;
void main(){ float t=clamp(vUV.y,0.0,1.0); fragColor = vec4(mix(uBottomGray,uTopBlack,t),1.0); }
)GLSL";

static const char* grid_vs_src = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos; uniform mat4 uMVP; out vec3 vWorldPos;
void main(){ vWorldPos = aPos; gl_Position = uMVP * vec4(aPos,1.0); }
)GLSL";
static const char* grid_fs_src = R"GLSL(
#version 330 core
in vec3 vWorldPos; out vec4 fragColor;
uniform vec3 uGridColor; uniform vec3 uAxisColorX; uniform vec3 uAxisColorY; uniform vec3 uAxisColorZ;
uniform float uCellSize; uniform int uMajorEveryN; uniform float uLineThickness; uniform float uAxisThicknessFactor;
const float MIN_FW=1e-4, MAX_FW=0.02, LINE_PIXEL_SCALE=1.5;
float gridLineMask(float coord, float thicknessInCells){
  float dist = abs(coord-round(coord)); float px = clamp(fwidth(coord), MIN_FW, MAX_FW);
  float thresh = max(1e-6, thicknessInCells * px * LINE_PIXEL_SCALE);
  return 1.0 - smoothstep(0.0, thresh, dist);
}
void main(){
  vec2 g = vWorldPos.xz / max(uCellSize, 1e-6);
  float minorX = gridLineMask(g.x, uLineThickness);
  float minorZ = gridLineMask(g.y, uLineThickness);
  float minorMask = max(minorX, minorZ);
  float majorMask = 0.0;
  if (uMajorEveryN>1){
    float ix = round(g.x), iz = round(g.y);
    if (mod(ix, float(uMajorEveryN))==0.0) majorMask = max(majorMask, gridLineMask(g.x, uLineThickness*2.0));
    if (mod(iz, float(uMajorEveryN))==0.0) majorMask = max(majorMask, gridLineMask(g.y, uLineThickness*2.0));
  }
  float baseMask = max(minorMask, majorMask);
  float axisThresh = uLineThickness * uAxisThicknessFactor * 0.5;
  float axisX = 1.0 - smoothstep(0.0, axisThresh, abs(vWorldPos.z));
  float axisZ = 1.0 - smoothstep(0.0, axisThresh, abs(vWorldPos.x));
  float originMask = 1.0 - smoothstep(0.0, uCellSize * 0.15, length(vWorldPos.xz));
  float axisDom = max(axisX, axisZ);
  vec3 finalColor = uGridColor;
  if (originMask>0.001) finalColor = uAxisColorY;
  else if (axisDom>0.01) finalColor = (axisX>=axisZ)?uAxisColorX:uAxisColorZ;
  else finalColor = (majorMask>0.5)?uGridColor*0.65:uGridColor;
  float linePresence = max(baseMask, max(max(axisX, axisZ), originMask));
  float fw = clamp(max(fwidth(g.x), fwidth(g.y)), MIN_FW, MAX_FW);
  float edge = fw * 0.75;
  float alpha = smoothstep(0.0, edge, linePresence);
  fragColor = vec4(finalColor, alpha);
}
)GLSL";

static const char* ui_vs_src = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos; layout(location=1) in vec2 aUV; out vec2 vUV;
void main(){ vUV=aUV; gl_Position=vec4(aPos,0,1); }
)GLSL";
static const char* ui_fs_src = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor; uniform vec4 uColor;
void main(){ fragColor = uColor; }
)GLSL";

// ---------- Impl (PIMPL style) ----------
struct App::Impl {
    int argc;
    char** argv;
    GLFWwindow* window = nullptr;

    // GL programs + VAOs
    GLuint prog = 0;
    GLuint bg_prog = 0;
    GLuint grid_prog = 0;
    GLuint ui_prog = 0;
    GLuint bg_vao = 0;
    GLuint grid_vao = 0;
    GLuint grid_vbo = 0;
    GLuint grid_ebo = 0;
    GLuint ui_vao = 0;
    GLuint ui_vbo = 0;

    // model GPU handles
    GLuint model_vao = 0;
    GLuint model_vbo = 0;
    GLuint model_ebo = 0;
    size_t model_index_count = 0;

    // uniforms
    GLint uMVP = -1, uModel = -1;
    GLint grid_uMVP = -1;

    // Camera / orbit
    double lastX = 0.0, lastY = 0.0; bool firstMouse = true;
    float yaw = glm::radians(-45.0f), pitch = glm::radians(25.0f);
    float distance = 6.0f;
    glm::vec3 target = glm::vec3(0.0f);
    CameraState camState;

    // Async model data (parsed in background)
    std::shared_ptr<std::vector<glm::vec3>> positions_ptr = std::make_shared<std::vector<glm::vec3>>();
    std::shared_ptr<std::vector<glm::vec3>> normals_ptr = std::make_shared<std::vector<glm::vec3>>();
    std::shared_ptr<std::vector<unsigned int>> indices_ptr = std::make_shared<std::vector<unsigned int>>();

    std::future<void> initialLoader;
    // import state (for requestImport)
    std::future<void> importLoaderFuture;
    std::shared_ptr<std::vector<glm::vec3>> import_positions_ptr;
    std::shared_ptr<std::vector<glm::vec3>> import_normals_ptr;
    std::shared_ptr<std::vector<unsigned int>> import_indices_ptr;
    std::shared_ptr<std::atomic<bool>> import_ready;
    std::shared_ptr<std::atomic<bool>> import_failed;
    std::shared_ptr<std::atomic<float>> import_progress;

    // state flags
    std::atomic<bool> modelReady{false};
    std::atomic<bool> modelLoadFailed{false};
    std::atomic<float> loadProgress{0.0f};
    bool modelUploaded = false;

    Impl(int a, char** v): argc(a), argv(v) {}
    ~Impl(){ /* cleanup in App::~App */ }

    bool initWindowAndGL() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, 16);
        window = glfwCreateWindow(1280, 720, "Splender 0.3.1", nullptr, nullptr);
        if (!window) { glfwTerminate(); return false; }

        // exe dir
        std::string exeDir;
#if defined(_WIN32)
        char exePathBuf[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
        if (len != 0 && len < MAX_PATH) { exeDir = std::filesystem::path(exePathBuf).parent_path().string(); }
        else exeDir = ".";
#else
        exeDir = std::filesystem::current_path().string();
#endif
        // try icon
        std::filesystem::path iconPath = std::filesystem::path(exeDir) / "assets" / "icons" / "splender_logo.png";
        int iw=0, ih=0, in=0;
        unsigned char* icon_pixels = stbi_load(iconPath.string().c_str(), &iw, &ih, &in, 4);
        if (icon_pixels) { GLFWimage images[1]; images[0].width=iw; images[0].height=ih; images[0].pixels=icon_pixels; glfwSetWindowIcon(window,1,images); stbi_image_free(icon_pixels); }

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
        prog = link_program(compile_shader(GL_VERTEX_SHADER, vs_src), compile_shader(GL_FRAGMENT_SHADER, fs_src));
        if (!prog) return false;
        uMVP = glGetUniformLocation(prog, "uMVP");
        uModel = glGetUniformLocation(prog, "uModel");

        bg_prog = link_program(compile_shader(GL_VERTEX_SHADER, bg_vs_src), compile_shader(GL_FRAGMENT_SHADER, bg_fs_src));
        if (bg_prog) { glGenVertexArrays(1, &bg_vao); }
        grid_prog = link_program(compile_shader(GL_VERTEX_SHADER, grid_vs_src), compile_shader(GL_FRAGMENT_SHADER, grid_fs_src));
        if (grid_prog) {
            glGenVertexArrays(1, &grid_vao);
            glGenBuffers(1, &grid_vbo);
            glGenBuffers(1, &grid_ebo);
            const float R = 10.0f;
            float gridVerts[] = { -R,0,-R, R,0,-R, R,0,R, -R,0,R };
            unsigned int gridIdx[] = {0,1,2, 0,2,3};
            glBindVertexArray(grid_vao);
            glBindBuffer(GL_ARRAY_BUFFER, grid_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(gridVerts), gridVerts, GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, grid_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(gridIdx), gridIdx, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);
            glBindVertexArray(0);
            grid_uMVP = glGetUniformLocation(grid_prog, "uMVP");
        }

        // UI quad
        glGenVertexArrays(1, &ui_vao);
        glGenBuffers(1, &ui_vbo);
        glBindVertexArray(ui_vao);
        float uiVerts[] = {
            -1.0f,-1.0f, 0.0f,0.0f,
             1.0f,-1.0f, 1.0f,0.0f,
             1.0f, 1.0f, 1.0f,1.0f,
            -1.0f,-1.0f, 0.0f,0.0f,
             1.0f, 1.0f, 1.0f,1.0f,
            -1.0f, 1.0f, 0.0f,1.0f
        };
        glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(uiVerts), uiVerts, GL_STATIC_DRAW);
        ui_prog = link_program(compile_shader(GL_VERTEX_SHADER, ui_vs_src), compile_shader(GL_FRAGMENT_SHADER, ui_fs_src));
        glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
        glBindVertexArray(0);

        return true;
    }

    void startInitialLoad(const std::string& model_path) {
        isLoading.store(true);
        positions_ptr = std::make_shared<std::vector<glm::vec3>>();
        normals_ptr = std::make_shared<std::vector<glm::vec3>>();
        indices_ptr = std::make_shared<std::vector<unsigned int>>();
        modelLoadFailed.store(false);
        modelReady.store(false);
        loadProgress.store(0.0f);

        initialLoader = std::async(std::launch::async,
            [this, model_path]() {
                bool ok = load_obj_simple(model_path, *positions_ptr, *normals_ptr, *indices_ptr, &loadProgress);
                if (!ok) modelLoadFailed.store(true);
                else modelReady.store(true);
                isLoading.store(false);
            });
    }

    void requestImportAsync(const std::string& path) {
        // Prepare import containers and shared flags
        import_positions_ptr = std::make_shared<std::vector<glm::vec3>>();
        import_normals_ptr = std::make_shared<std::vector<glm::vec3>>();
        import_indices_ptr = std::make_shared<std::vector<unsigned int>>();
        import_ready = std::make_shared<std::atomic<bool>>(false);
        import_failed = std::make_shared<std::atomic<bool>>(false);
        import_progress = std::make_shared<std::atomic<float>>(0.0f);

        isLoading.store(true);

        importLoaderFuture = std::async(std::launch::async,
            [this, path]() {
                bool ok = load_obj_simple(path, *import_positions_ptr, *import_normals_ptr, *import_indices_ptr, import_progress.get());
                if (!ok) import_failed->store(true);
                else import_ready->store(true);
                isLoading.store(false);
            });
    }

    // Called each frame on main thread to swap in import when ready
    void maybeFinishImport() {
        if (import_ready && import_ready->load()) {
            if (import_failed && import_failed->load()) {
                std::cerr << "Import model failed to parse\n";
            } else {
                // delete old GL buffers
                if (model_ebo) { glDeleteBuffers(1, &model_ebo); model_ebo = 0; }
                if (model_vbo) { glDeleteBuffers(1, &model_vbo); model_vbo = 0; }
                if (model_vao) { glDeleteVertexArrays(1, &model_vao); model_vao = 0; }
                modelUploaded = false;

                positions_ptr = std::move(import_positions_ptr);
                normals_ptr = std::move(import_normals_ptr);
                indices_ptr = std::move(import_indices_ptr);

                modelReady.store(true);
                modelLoadFailed.store(false);
                loadProgress.store(import_progress ? import_progress->load() : 0.0f);
            }

            import_ready.reset(); import_failed.reset(); import_progress.reset();
            if (importLoaderFuture.valid()) importLoaderFuture = std::future<void>();
        }
    }

    void uploadModelIfReady() {
        if (!modelUploaded && modelReady.load()) {
            // Build interleaved array
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
            modelUploaded = true;
        }
    }

    void shutdownCleanup() {
        // GL cleanup (called when context still valid)
        if (model_ebo) { glDeleteBuffers(1, &model_ebo); model_ebo = 0; }
        if (model_vbo) { glDeleteBuffers(1, &model_vbo); model_vbo = 0; }
        if (model_vao) { glDeleteVertexArrays(1, &model_vao); model_vao = 0; }
        if (ui_vbo) { glDeleteBuffers(1, &ui_vbo); ui_vbo = 0; }
        if (ui_vao) { glDeleteVertexArrays(1, &ui_vao); ui_vao = 0; }
        if (grid_ebo) { glDeleteBuffers(1, &grid_ebo); grid_ebo = 0; }
        if (grid_vbo) { glDeleteBuffers(1, &grid_vbo); grid_vbo = 0; }
        if (grid_vao) { glDeleteVertexArrays(1, &grid_vao); grid_vao = 0; }
        if (bg_vao) { glDeleteVertexArrays(1, &bg_vao); bg_vao = 0; }
        if (prog) { glDeleteProgram(prog); prog = 0; }
        if (bg_prog) { glDeleteProgram(bg_prog); bg_prog = 0; }
        if (grid_prog) { glDeleteProgram(grid_prog); grid_prog = 0; }
        if (ui_prog) { glDeleteProgram(ui_prog); ui_prog = 0; }
    }
};

// ---------- App public methods ----------

App::App(int argc, char** argv) : impl_(std::make_unique<Impl>(argc, argv)) {}

App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;

App::~App() {
    // Ensure we shut down and free GL/ImGui with a valid context if still running.
    if (impl_) {
        // if window still exists, perform GL cleanup
        if (impl_->window) {
            // Make context current to clean GL resources
            glfwMakeContextCurrent(impl_->window);
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
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
    if (!I.compileBuiltinPrograms()) return -1;

    // determine initial model path
    std::string model_path = std::filesystem::path(std::filesystem::current_path() / "assets" / "splender.obj").string();
    if (I.argc > 1) model_path = std::string(I.argv[1]);
    std::cout << "Model path: " << model_path << "\n";

    // start initial async load
    I.startInitialLoad(model_path);

    // frame loop
    while (!glfwWindowShouldClose(I.window)) {
        // input
        double mx, my; glfwGetCursorPos(I.window, &mx, &my);
        int middleState = glfwGetMouseButton(I.window, GLFW_MOUSE_BUTTON_MIDDLE);
        bool altState = (glfwGetKey(I.window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS) || (glfwGetKey(I.window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS);
        if (I.firstMouse) { I.lastX = mx; I.lastY = my; I.firstMouse = false; }

        if (!isLoading.load()) {
            if (middleState == GLFW_PRESS) {
                double dx = mx - I.lastX, dy = my - I.lastY;
                if (altState) {
                    I.yaw += float(dx) * 0.005f;
                    I.pitch += float(dy) * 0.005f;
                    const float pl = glm::radians(89.0f); I.pitch = glm::clamp(I.pitch, -pl, pl);
                } else {
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

        // UI new frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Menu bar + Import action
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                ImGui::BeginDisabled(isLoading.load());
                if (ImGui::MenuItem("Import")) {
#if defined(_WIN32)
                    char szFile[4096] = {};
                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = nullptr;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = (DWORD)sizeof(szFile);
                    ofn.lpstrFilter = "Wavefront OBJ\0*.obj;*.OBJ\0All files\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameA(&ofn)) {
                        const std::string chosenPath = std::string(ofn.lpstrFile);
                        I.requestImportAsync(chosenPath);
                    }
#else
                    // Non-Windows: unimplemented
#endif
                }
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Draw background
        if (I.bg_prog) {
            glUseProgram(I.bg_prog);
            GLint locBottom = glGetUniformLocation(I.bg_prog, "uBottomGray");
            GLint locTop = glGetUniformLocation(I.bg_prog, "uTopBlack");
            if (locBottom >= 0) glUniform3f(locBottom, 0.45f, 0.45f, 0.45f);
            if (locTop >= 0) glUniform3f(locTop, 0.0f, 0.0f, 0.0f);

            GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
            GLboolean prevDepthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
            if (wasDepthTest) glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glBindVertexArray(I.bg_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            if (wasDepthTest) glEnable(GL_DEPTH_TEST);
            glDepthMask(prevDepthMask);
            glUseProgram(0);
        }

        glClear(GL_DEPTH_BUFFER_BIT);

        // camera
        float cx = I.distance * cos(I.pitch) * cos(I.yaw);
        float cy = I.distance * sin(I.pitch);
        float cz = I.distance * cos(I.pitch) * sin(I.yaw);
        glm::vec3 camPos = I.target + glm::vec3(cx, cy, cz);
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), fbW>0 ? (float)fbW/fbH : 1.0f, 0.01f, 1000.0f);
        glm::mat4 view = glm::lookAt(camPos, I.target, glm::vec3(0,1,0));
        glm::mat4 model = glm::mat4(1.0f);
        glm::mat4 mvp = proj * view * model;

        // Handle import completion
        I.maybeFinishImport();

        // Upload model if parsed
        I.uploadModelIfReady();

        // Draw model
        if (I.prog) {
            glUseProgram(I.prog);
            if (I.uMVP >= 0) glUniformMatrix4fv(I.uMVP, 1, GL_FALSE, glm::value_ptr(mvp));
            if (I.uModel >= 0) glUniformMatrix4fv(I.uModel, 1, GL_FALSE, glm::value_ptr(model));
            if (I.modelUploaded) {
                glBindVertexArray(I.model_vao);
                glDrawElements(GL_TRIANGLES, (GLsizei)I.model_index_count, GL_UNSIGNED_INT, 0);
                glBindVertexArray(0);
            }
            glUseProgram(0);
        }

        // Draw grid
        if (I.grid_prog) {
            glm::mat4 mvpGrid = proj * view * glm::mat4(1.0f);
            glUseProgram(I.grid_prog);
            if (I.grid_uMVP >= 0) glUniformMatrix4fv(I.grid_uMVP, 1, GL_FALSE, glm::value_ptr(mvpGrid));
            GLint loc = glGetUniformLocation(I.grid_prog, "uGridColor"); if (loc>=0) glUniform3f(loc, 0.6f,0.6f,0.6f);
            loc = glGetUniformLocation(I.grid_prog, "uAxisColorX"); if (loc>=0) glUniform3f(loc, 0.79f,0.24f,0.28f);
            loc = glGetUniformLocation(I.grid_prog, "uAxisColorY"); if (loc>=0) glUniform3f(loc, 0.52f,0.84f,0.40f);
            loc = glGetUniformLocation(I.grid_prog, "uAxisColorZ"); if (loc>=0) glUniform3f(loc, 0.47f,0.79f,0.24f);
            loc = glGetUniformLocation(I.grid_prog, "uCellSize"); if (loc>=0) glUniform1f(loc, 1.0f);
            loc = glGetUniformLocation(I.grid_prog, "uMajorEveryN"); if (loc>=0) glUniform1i(loc, 10);
            loc = glGetUniformLocation(I.grid_prog, "uLineThickness"); if (loc>=0) glUniform1f(loc, 0.5f);
            loc = glGetUniformLocation(I.grid_prog, "uAxisThicknessFactor"); if (loc>=0) glUniform1f(loc, 0.1f);

            GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
            GLboolean prevDepthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
            glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
            glBindVertexArray(I.grid_vao);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
            glDepthMask(prevDepthMask);
            if (!wasDepthTest) glDisable(GL_DEPTH_TEST);
            glUseProgram(0);
        }

        // Loading modal
        if (isLoading.load()) {
            GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
            GLboolean prevDepthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
            if (wasDepthTest) glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);

            float frac = 0.0f;
            if (I.import_progress) frac = I.import_progress->load();
            else frac = I.loadProgress.load();
            frac = glm::clamp(frac, 0.0f, 1.0f);

            ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
            int fbW2, fbH2; glfwGetFramebufferSize(I.window, &fbW2, &fbH2);
            int boxW = (int)(fbW2 * 0.5f);
            ImVec2 winSize((float)boxW, 96.0f);
            ImVec2 winPos((fbW2 - boxW) * 0.5f, (fbH2 - (int)winSize.y) * 0.5f);
            ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.92f);

            ImGui::Begin("LoadingModal", nullptr, wf);
            ImGui::TextColored(ImVec4(0.9f,0.9f,0.9f,1.0f), "Loading model...");
            ImGui::Dummy(ImVec2(0.0f,6.0f));
            ImGui::ProgressBar(frac, ImVec2((float)boxW - 24.0f, 18.0f));
            ImGui::Dummy(ImVec2(0.0f,6.0f));
            ImGui::SameLine();
            ImGui::Text("%d%%", (int)std::round(frac*100.0f));
            ImGui::End();

            if (wasDepthTest) glEnable(GL_DEPTH_TEST);
            glDepthMask(prevDepthMask);
        }

        // Render ImGui
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(I.window);
        glfwPollEvents();
    }

    // wait for initial loader
    if (I.initialLoader.valid()) I.initialLoader.wait();

    // shutdown: destroy GL resources and ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

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
    // no import while importing
    if (isLoading.load()) return;
    impl_->requestImportAsync(objPath);
}

void App::shutdown() {
    // set a flag main loop will exit in response to window close or this flag.
    if (impl_ && impl_->window) glfwSetWindowShouldClose(impl_->window, GLFW_TRUE);
}
