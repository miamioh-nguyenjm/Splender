#pragma once

// app.h
// Public interface for the application

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <future>

#include <glm/glm.hpp>

struct CameraState {
    float* distance;
    float  minDistance;
    float  maxDistance;
    float  zoomSpeed;
};

// OBJ loader signature (implemented in loader.cpp)
bool load_model_simple(const std::string& path,
                       std::vector<glm::vec3>& out_positions,
                       std::vector<glm::vec3>& out_normals,
                       std::vector<unsigned int>& out_indices,
                       std::atomic<float>* progress = nullptr);

// Global flag used across modules to indicate a background model load is active.
extern std::atomic<bool> isLoading;

class App {
public:
    // Construct with process args (same as main signature) (open with windows?)
    App(int argc, char** argv);

    int run();

    void requestImport(const std::string& objPath);

    void shutdown();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    ~App();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
