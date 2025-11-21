// loader.cpp
// Implements Loader declared in loader.h

#include "loader.h"

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <future>
#include <memory>
#include <atomic>
#include <filesystem>
#include <cctype>

#include <glm/glm.hpp>

#ifdef USE_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#endif

Loader::Loader()
{
    positions_ptr = std::make_shared<std::vector<glm::vec3>>();
    normals_ptr = std::make_shared<std::vector<glm::vec3>>();
    indices_ptr = std::make_shared<std::vector<unsigned int>>();
    modelReady.store(false);
    modelLoadFailed.store(false);
    loadProgress.store(0.0f);
}

Loader::~Loader()
{
    if (initialLoader.valid()) initialLoader.wait();
    if (importLoaderFuture.valid()) importLoaderFuture.wait();
}

static std::string extlower(const std::string& p) {
    auto s = std::filesystem::path(p).extension().string();
    for (auto &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

// Forward to internal OBJ parser used below
static bool load_obj_simple_internal(const std::string& path,
                        std::vector<glm::vec3>& out_positions,
                        std::vector<glm::vec3>& out_normals,
                        std::vector<unsigned int>& out_indices,
                        std::atomic<float>* progress);

void Loader::startInitialLoad(const std::string& model_path) {
    positions_ptr = std::make_shared<std::vector<glm::vec3>>();
    normals_ptr = std::make_shared<std::vector<glm::vec3>>();
    indices_ptr = std::make_shared<std::vector<unsigned int>>();
    modelLoadFailed.store(false);
    modelReady.store(false);
    loadProgress.store(0.0f);

    initialLoader = std::async(std::launch::async,
        [this, model_path]() {
            bool ok = Loader::load_model_simple(model_path, *positions_ptr, *normals_ptr, *indices_ptr, &loadProgress);
            if (!ok) modelLoadFailed.store(true);
            else modelReady.store(true);
        });
}

void Loader::requestImportAsync(const std::string& path) {
    import_positions_ptr = std::make_shared<std::vector<glm::vec3>>();
    import_normals_ptr = std::make_shared<std::vector<glm::vec3>>();
    import_indices_ptr = std::make_shared<std::vector<unsigned int>>();
    import_ready = std::make_shared<std::atomic<bool>>(false);
    import_failed = std::make_shared<std::atomic<bool>>(false);
    import_progress = std::make_shared<std::atomic<float>>(0.0f);

    importLoaderFuture = std::async(std::launch::async,
        [this, path]() {
            bool ok = Loader::load_model_simple(path, *import_positions_ptr, *import_normals_ptr, *import_indices_ptr, import_progress.get());
            if (!ok) import_failed->store(true);
            else import_ready->store(true);
        });
}

bool Loader::maybeFinishImport() {
    if (import_ready && import_ready->load()) {
        if (import_failed && import_failed->load()) {
            std::cerr << "Import model failed to parse\n";
        } else {
            positions_ptr = std::move(import_positions_ptr);
            normals_ptr = std::move(import_normals_ptr);
            indices_ptr = std::move(import_indices_ptr);

            modelReady.store(true);
            modelLoadFailed.store(false);
            loadProgress.store(import_progress ? import_progress->load() : 0.0f);
        }

        import_ready.reset(); import_failed.reset(); import_progress.reset();
        if (importLoaderFuture.valid()) importLoaderFuture = std::future<void>();
        return true;
    }
    return false;
}

bool Loader::load_model_simple(const std::string& path,
                               std::vector<glm::vec3>& out_positions,
                               std::vector<glm::vec3>& out_normals,
                               std::vector<unsigned int>& out_indices,
                               std::atomic<float>* progress)
{
    const std::string ext = extlower(path);
    if (ext == ".obj") {
        return load_obj_simple_internal(path, out_positions, out_normals, out_indices, progress);
    }

#ifdef USE_ASSIMP
    if (ext == ".fbx" || ext == ".dae" || ext == ".gltf" || ext == ".glb" || ext == ".ply" || ext == ".stl") {
        if (progress) progress->store(0.0f);

        Assimp::Importer importer;
        unsigned int flags = aiProcess_Triangulate
                           | aiProcess_GenSmoothNormals
                           | aiProcess_JoinIdenticalVertices
                           | aiProcess_ImproveCacheLocality
                           | aiProcess_RemoveRedundantMaterials
                           | aiProcess_PreTransformVertices;

        const aiScene* scene = importer.ReadFile(path, flags);
        if (!scene || !scene->HasMeshes()) {
            std::cerr << "Assimp failed to load " << path << ": " << importer.GetErrorString() << "\n";
            if (progress) progress->store(1.0f);
            return false;
        }

        out_positions.clear();
        out_normals.clear();
        out_indices.clear();

        struct Key { glm::vec3 p,n; bool operator==(Key const& o) const { return p==o.p && n==o.n; } };
        struct KeyHash { size_t operator()(Key const& k) const noexcept {
            size_t h1 = std::hash<float>()(k.p.x) ^ (std::hash<float>()(k.p.y) << 1) ^ (std::hash<float>()(k.p.z) << 2);
            size_t h2 = std::hash<float>()(k.n.x) ^ (std::hash<float>()(k.n.y) << 1) ^ (std::hash<float>()(k.n.z) << 2);
            return h1 ^ (h2 << 1);
        } };
        std::unordered_map<Key, unsigned int, KeyHash> vertMap;
        vertMap.reserve(1024);

        for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
            const aiMesh* mesh = scene->mMeshes[m];
            if (progress) progress->store(float(m) / float(scene->mNumMeshes + 1));
            for (unsigned i = 0; i < mesh->mNumFaces; ++i) {
                const aiFace& f = mesh->mFaces[i];
                if (f.mNumIndices != 3) continue;
                for (unsigned k = 0; k < 3; ++k) {
                    unsigned int idx = f.mIndices[k];
                    glm::vec3 p(0.0f), n(0.0f);
                    if (mesh->HasPositions()) {
                        aiVector3D pp = mesh->mVertices[idx];
                        p = glm::vec3(pp.x, pp.y, pp.z);
                    }
                    if (mesh->HasNormals()) {
                        aiVector3D nn = mesh->mNormals[idx];
                        n = glm::vec3(nn.x, nn.y, nn.z);
                    } else {
                        n = glm::vec3(0.0f, 0.0f, 1.0f);
                    }
                    Key kkey{p,n};
                    auto it = vertMap.find(kkey);
                    if (it != vertMap.end()) {
                        out_indices.push_back(it->second);
                    } else {
                        unsigned int newIndex = (unsigned int)out_positions.size();
                        vertMap.emplace(kkey, newIndex);
                        out_positions.push_back(p);
                        out_normals.push_back(n);
                        out_indices.push_back(newIndex);
                    }
                }
            }
        }

        if (!out_positions.empty()) {
            glm::vec3 minP = out_positions[0];
            glm::vec3 maxP = out_positions[0];
            for (size_t i = 1; i < out_positions.size(); ++i) {
                minP = glm::min(minP, out_positions[i]);
                maxP = glm::max(maxP, out_positions[i]);
            }
            glm::vec3 diag = maxP - minP;
            float maxDim = glm::max(glm::max(diag.x, diag.y), diag.z);
            if (maxDim > 1e-6f) {
                const float targetSize = 1.0f;
                float scale = targetSize / maxDim * 10.0f;
                for (auto &p : out_positions) p *= scale;
            }
        }

        if (progress) progress->store(1.0f);
        return !out_indices.empty();
    }
#endif // USE_ASSIMP

    // Unknown extension: try OBJ fallback
    if (ext.empty()) {
        return load_obj_simple_internal(path, out_positions, out_normals, out_indices, progress);
    }

    std::cerr << "Unsupported model extension: " << ext << " for path " << path << "\n";
    if (progress) progress->store(1.0f);
    return false;
}

// old OBJ parser. remains as only dedicated model parser outside of assimp
static bool load_obj_simple_internal(const std::string& path,
                        std::vector<glm::vec3>& out_positions,
                        std::vector<glm::vec3>& out_normals,
                        std::vector<unsigned int>& out_indices,
                        std::atomic<float>* progress)
{
    std::vector<glm::vec3> temp_pos;
    std::vector<glm::vec3> temp_norm;
    std::vector<unsigned int> pos_idx, norm_idx;

    std::ifstream in(path);
    if (!in) {
        std::cerr << "failed to open OBJ: " << path << "\n";
        return false;
    }

    in.seekg(0, std::ios::end);
    size_t fileBytes = (size_t)in.tellg();
    in.seekg(0, std::ios::beg);
    size_t approxLines = (fileBytes > 0) ? (fileBytes / 48) : 1024;
    temp_pos.reserve(approxLines / 4);
    temp_norm.reserve(approxLines / 8);
    pos_idx.reserve(approxLines);
    norm_idx.reserve(approxLines);

    if (progress) progress->store(0.0f);

    std::string line;
    size_t bytesSeen = 0;
    size_t lastProgressUpdateBytes = 0;
    const size_t PROGRESS_UPDATE_GRANULARITY = (1u << 12);

    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "v") {
            glm::vec3 p; ss >> p.x >> p.y >> p.z;
            temp_pos.push_back(p);
        } else if (tag == "vn") {
            glm::vec3 n; ss >> n.x >> n.y >> n.z;
            temp_norm.push_back(n);
        } else if (tag == "f") {
            std::vector<int> face_pos_idx;
            std::vector<int> face_norm_idx;
            face_pos_idx.reserve(8);
            face_norm_idx.reserve(8);

            std::string vert;
            while (ss >> vert) {
                int vi = 0, ni = 0;
                const char* s = vert.c_str();
                char* endptr = nullptr;
                long vval = strtol(s, &endptr, 10);
                if (endptr == s) continue;
                vi = (int)vval;
                if (*endptr == '/') {
                    const char* p2 = endptr + 1;
                    if (*p2 == '/') {
                        const char* p3 = p2 + 1;
                        ni = (int)strtol(p3, nullptr, 10);
                    } else {
                        char* endptr2 = nullptr;
                        strtol(p2, &endptr2, 10);
                        if (*endptr2 == '/') {
                            ni = (int)strtol(endptr2 + 1, nullptr, 10);
                        }
                    }
                }

                auto convert_index = [](int idx, size_t array_size) -> int {
                    if (idx > 0) return idx - 1;
                    if (idx < 0) return (int)array_size + idx;
                    return -1;
                };

                int posIndex = convert_index(vi, temp_pos.size());
                int normIndex = convert_index(ni, temp_norm.size());

                face_pos_idx.push_back(posIndex >= 0 ? posIndex : -1);
                face_norm_idx.push_back(normIndex >= 0 ? normIndex : -1);
            }

            if (face_pos_idx.size() < 3) {
                // skip
            } else {
                for (size_t i = 2; i < face_pos_idx.size(); ++i) {
                    int p0 = face_pos_idx[0];
                    int p1 = face_pos_idx[i-1];
                    int p2 = face_pos_idx[i];
                    int n0 = face_norm_idx[0];
                    int n1 = face_norm_idx[i-1];
                    int n2 = face_norm_idx[i];

                    pos_idx.push_back((p0 >= 0) ? (unsigned int)p0 : 0u);
                    pos_idx.push_back((p1 >= 0) ? (unsigned int)p1 : 0u);
                    pos_idx.push_back((p2 >= 0) ? (unsigned int)p2 : 0u);

                    norm_idx.push_back((n0 >= 0) ? (unsigned int)n0 : 0u);
                    norm_idx.push_back((n1 >= 0) ? (unsigned int)n1 : 0u);
                    norm_idx.push_back((n2 >= 0) ? (unsigned int)n2 : 0u);
                }
            }
        }

        bytesSeen += line.size() + 1;
        if (progress && (bytesSeen - lastProgressUpdateBytes >= PROGRESS_UPDATE_GRANULARITY)) {
            float p = (fileBytes > 0) ? std::min(1.0f, float(bytesSeen) / float(fileBytes)) : 0.0f;
            progress->store(p);
            lastProgressUpdateBytes = bytesSeen;
        }
    }

    struct Key { int p, n; bool operator==(Key const& o) const { return p == o.p && n == o.n; } };
    struct KeyHash { size_t operator()(Key const& k) const noexcept { return (size_t)k.p * 1000003u + (size_t)k.n; } };

    std::unordered_map<Key, unsigned int, KeyHash> map;
    map.reserve(pos_idx.size() * 2);

    out_positions.clear();
    out_normals.clear();
    out_indices.clear();
    out_positions.reserve(pos_idx.size());
    out_normals.reserve(pos_idx.size());
    out_indices.reserve(pos_idx.size());

    for (size_t i = 0; i < pos_idx.size(); ++i) {
        Key key{ (int)pos_idx[i], (int)norm_idx[i] };
        auto it = map.find(key);
        if (it != map.end()) {
            out_indices.push_back(it->second);
        } else {
            unsigned int newIndex = (unsigned int)out_positions.size();
            map[key] = newIndex;
            out_positions.push_back((temp_pos.size() > (size_t)key.p) ? temp_pos[key.p] : glm::vec3(0.0f));
            if (!temp_norm.empty() && (size_t)key.n < temp_norm.size()) out_normals.push_back(temp_norm[key.n]);
            else out_normals.push_back(glm::vec3(0.0f, 0.0f, 1.0f));
            out_indices.push_back(newIndex);
        }
    }

    if (progress) progress->store(1.0f);
    return true;
}

// free function ABI expected by older code forward to Loaderload_model_simple
bool load_model_simple(const std::string& path,
                       std::vector<glm::vec3>& out_positions,
                       std::vector<glm::vec3>& out_normals,
                       std::vector<unsigned int>& out_indices,
                       std::atomic<float>* progress)
{
    return Loader::load_model_simple(path, out_positions, out_normals, out_indices, progress);
}
