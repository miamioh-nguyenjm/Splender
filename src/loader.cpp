// loader.cpp
// Exposes: load_obj_simple(...) and start_async_obj_load(...)

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
#include "loader.h"

#include <glm/glm.hpp>

// Parses a simple OBJ (triangulated faces). Produces de-duplicated indexed vertex arrays.
// - path: filesystem path to OBJ
// - out_positions: per-vertex positions (matches out_normals length)
// - out_normals: per-vertex normals (if OBJ had no normals, all (0,0,1))
// - out_indices: triangle indices referencing out_positions/out_normals
// - progress: optional atomic<float> pointer updated 0..1 while parsing
// Returns true on success.
bool load_obj_simple(const std::string& path,
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

    // Estimate sizes to reduce reallocations
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
    const size_t PROGRESS_UPDATE_GRANULARITY = (1u << 12); // ~4KB

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
    // faces: support arbitrary polygons. Triangulate using fan (v0, v[i-1], v[i]).
    std::vector<int> face_pos_idx;
    std::vector<int> face_norm_idx;
    face_pos_idx.reserve(8);
    face_norm_idx.reserve(8);

    std::string vert;
    while (ss >> vert) {
        int vi = 0, ni = 0;
        const char* s = vert.c_str();
        char* endptr = nullptr; 

        // parse v (may be negative)
        long vval = strtol(s, &endptr, 10);
        if (endptr == s) {
            // malformed vertex token, skip it
            continue;
        }
        vi = (int)vval;

        if (*endptr == '/') {
            const char* p2 = endptr + 1;
            if (*p2 == '/') {
                // v//vn
                const char* p3 = p2 + 1;
                ni = (int)strtol(p3, nullptr, 10);
            } else {
                // v/vt/vn or v/vt
                char* endptr2 = nullptr;
                strtol(p2, &endptr2, 10); // vt ignored
                if (*endptr2 == '/') {
                    ni = (int)strtol(endptr2 + 1, nullptr, 10);
                }
            }
        }

        // Convert OBJ indices (1-based, negatives allowed) to zero-based.
        auto convert_index = [](int idx, size_t array_size) -> int {
            if (idx > 0) return idx - 1;
            if (idx < 0) return (int)array_size + idx; // negative: relative to end
            return -1; // missing
        };

        int posIndex = convert_index(vi, temp_pos.size());
        int normIndex = convert_index(ni, temp_norm.size());

        // push into per-face temporary lists
        face_pos_idx.push_back(posIndex >= 0 ? posIndex : -1);
        face_norm_idx.push_back(normIndex >= 0 ? normIndex : -1);
    }

    // If face has fewer than 3 resolved positions, skip it
    if (face_pos_idx.size() < 3) {
        // nothing to do
    } else {
        // Triangulate fan: (0, i-1, i) for i=2..n-1
        for (size_t i = 2; i < face_pos_idx.size(); ++i) {
            int p0 = face_pos_idx[0];
            int p1 = face_pos_idx[i-1];
            int p2 = face_pos_idx[i];
            int n0 = face_norm_idx[0];
            int n1 = face_norm_idx[i-1];
            int n2 = face_norm_idx[i];

            // If any position index is invalid, substitute a safe 0 (caller later handles missing pos by default)
            pos_idx.push_back((p0 >= 0) ? (unsigned int)p0 : 0u);
            pos_idx.push_back((p1 >= 0) ? (unsigned int)p1 : 0u);
            pos_idx.push_back((p2 >= 0) ? (unsigned int)p2 : 0u);

            // For normals, push -1 -> will be handled when building out_normals (fallback to default normal)
            norm_idx.push_back((n0 >= 0) ? (unsigned int)n0 : 0u);
            norm_idx.push_back((n1 >= 0) ? (unsigned int)n1 : 0u);
            norm_idx.push_back((n2 >= 0) ? (unsigned int)n2 : 0u);
        }
    }
}


        // progress update
        bytesSeen += line.size() + 1;
        if (progress && (bytesSeen - lastProgressUpdateBytes >= PROGRESS_UPDATE_GRANULARITY)) {
            float p = (fileBytes > 0) ? std::min(1.0f, float(bytesSeen) / float(fileBytes)) : 0.0f;
            progress->store(p);
            lastProgressUpdateBytes = bytesSeen;
        }
    }

    // Build unique (position, normal) keys and flatten into GPU-friendly arrays
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
            // safe access: assume temp_pos/temp_norm are populated or provide defaults
            out_positions.push_back(temp_pos.size() > (size_t)key.p ? temp_pos[key.p] : glm::vec3(0.0f));
            if (!temp_norm.empty() && (size_t)key.n < temp_norm.size()) out_normals.push_back(temp_norm[key.n]);
            else out_normals.push_back(glm::vec3(0.0f, 0.0f, 1.0f));
            out_indices.push_back(newIndex);
        }
    }

    if (progress) progress->store(1.0f);
    return true;
}

// Async helper: starts parsing on background thread and returns a future that completes when parsing finished.
// The shared_ptrs are filled by the background task so the caller can later consume them (on main thread) safely.
struct AsyncLoadResult {
    std::shared_ptr<std::vector<glm::vec3>> positions;
    std::shared_ptr<std::vector<glm::vec3>> normals;
    std::shared_ptr<std::vector<unsigned int>> indices;
    std::shared_ptr<std::atomic<float>> progress;
    std::shared_ptr<std::atomic<bool>> failed;
    std::future<void> future;
};

// Start an async OBJ load. Caller should keep returned AsyncLoadResult alive until future is ready and they consumed data.
AsyncLoadResult start_async_obj_load(const std::string& model_path)
{
    AsyncLoadResult r;
    r.positions = std::make_shared<std::vector<glm::vec3>>();
    r.normals   = std::make_shared<std::vector<glm::vec3>>();
    r.indices   = std::make_shared<std::vector<unsigned int>>();
    r.progress  = std::make_shared<std::atomic<float>>(0.0f);
    r.failed    = std::make_shared<std::atomic<bool>>(false);

    // Launch background parse
    r.future = std::async(std::launch::async, [model_path, pPos = r.positions, pNorm = r.normals, pIdx = r.indices, pProg = r.progress, pFail = r.failed]() {
        bool ok = load_obj_simple(model_path, *pPos, *pNorm, *pIdx, pProg.get());
        if (!ok) {
            pFail->store(true);
            std::cerr << "Failed to load OBJ (async): " << model_path << std::endl;
        }
        // progress set to 1.0 by loader on success
    });

    return r;
}
