#pragma once

// renderer.h

#include <vector>
#include <memory>
#include <atomic>

#include <glm/glm.hpp>

#include "globals.h" // extern std::atomic<bool> isLoading;

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    // Initialize GL state and compile built-in shaders. Must be called after GL context creation.
    // Returns true on success.
    bool init(int framebufferWidth, int framebufferHeight);

    // Resize viewport / projection targets.
    void resize(int framebufferWidth, int framebufferHeight);

    // Upload a model to GPU. Caller supplies positions, normals and triangle indices.
    // This call must be made on the main thread (GL thread).
    // Replaces any previously uploaded model.
    void uploadModel(const std::vector<glm::vec3>& positions,
                     const std::vector<glm::vec3>& normals,
                     const std::vector<unsigned int>& indices);

    // Draw the full scene: background, grid, model, and any overlays.
    // Camera/view/projection are set via setCamera.
    void draw();

    // Simple camera setter for orbit camera (position and target).
    void setCamera(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up,
                   float fovDegrees, float nearPlane, float farPlane);

    // Toggle grid and background rendering
    void setShowGrid(bool show);
    void setShowBackground(bool show);

    void setModelTint(const glm::vec3& rgb);

    // Cleanup GL resources. Safe to call multiple times.
    void shutdown();

    // Non-copyable
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
