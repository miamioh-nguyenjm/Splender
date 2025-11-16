#pragma once

// app.h
// Public interface for the application (extracted from main0

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
bool load_obj_simple(const std::string& path,
                     std::vector<glm::vec3>& out_positions,
                     std::vector<glm::vec3>& out_normals,
                     std::vector<unsigned int>& out_indices,
                     std::atomic<float>* progress = nullptr);

// Global flag used across modules to indicate a background model load is active.
extern std::atomic<bool> isLoading;

class App {
public:
    // Construct with process args (same as main signature). App assumes ownership of initialization.
    App(int argc, char** argv);

    // Run the main loop. Returns the process exit code.
    int run();

    // Request an async import of an OBJ file by path. Safe to call from UI callbacks.
    // This will start a background parse and swap the model in on completion on the main thread.
    void requestImport(const std::string& objPath);

    // Gracefully stop any background work and shutdown (callable from another thread).
    void shutdown();

    // Non-copyable, movable allowed for convenience
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    ~App();

private:
    // Implementation details live in app.cpp 
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
