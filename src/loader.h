#pragma once
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <glm/vec3.hpp>
#include <future>

bool load_model_simple(const std::string& path,
                       std::vector<glm::vec3>& out_positions,
                       std::vector<glm::vec3>& out_normals,
                       std::vector<unsigned int>& out_indices,
                       std::atomic<float>* progress);

// Loader class declared here, defined in loader.cpp
struct Loader {
    std::shared_ptr<std::vector<glm::vec3>> positions_ptr;
    std::shared_ptr<std::vector<glm::vec3>> normals_ptr;
    std::shared_ptr<std::vector<unsigned int>> indices_ptr;

    std::future<void> initialLoader;

    // import state
    std::future<void> importLoaderFuture;
    std::shared_ptr<std::vector<glm::vec3>> import_positions_ptr;
    std::shared_ptr<std::vector<glm::vec3>> import_normals_ptr;
    std::shared_ptr<std::vector<unsigned int>> import_indices_ptr;
    std::shared_ptr<std::atomic<bool>> import_ready;
    std::shared_ptr<std::atomic<bool>> import_failed;
    std::shared_ptr<std::atomic<float>> import_progress;

    std::atomic<bool> modelReady{false};
    std::atomic<bool> modelLoadFailed{false};
    std::atomic<float> loadProgress{0.0f};

    Loader();
    ~Loader();

    void startInitialLoad(const std::string& model_path);
    void requestImportAsync(const std::string& path);
    bool maybeFinishImport();

    std::shared_ptr<std::vector<glm::vec3>> positions() const { return positions_ptr; }
    std::shared_ptr<std::vector<glm::vec3>> normals() const { return normals_ptr; }
    std::shared_ptr<std::vector<unsigned int>> indices() const { return indices_ptr; }

    std::shared_ptr<std::atomic<float>> currentImportProgress() const { return import_progress; }

    static bool load_model_simple(const std::string& path,
                                  std::vector<glm::vec3>& out_positions,
                                  std::vector<glm::vec3>& out_normals,
                                  std::vector<unsigned int>& out_indices,
                                  std::atomic<float>* progress = nullptr);
};
