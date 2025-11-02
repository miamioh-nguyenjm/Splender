

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <thread>

// opengl loader
#include <glad/glad.h>

// for the window
#include <GLFW/glfw3.h>

// glm for linear algebra
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// here we read the vertex data of the obj. obj is stored as text so we can do this
bool load_obj_simple(const std::string& path,
                     std::vector<glm::vec3>& out_positions,
                     std::vector<glm::vec3>& out_normals,
                     std::vector<unsigned int>& out_indices)
{
    std::vector<glm::vec3> temp_pos;
    std::vector<glm::vec3> temp_norm;
    std::vector<unsigned int> pos_idx, norm_idx;

    std::ifstream in(path);
    if (!in) { std::cerr<<"failed to open  OBJ: "<<path<<"\n"; return false; }

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        // in obj, v is vertex, vn is vertex normals, and f is faces
        if (tag == "v") {
            glm::vec3 p; ss >> p.x >> p.y >> p.z; temp_pos.push_back(p);
        } else if (tag == "vn") {
            glm::vec3 n; ss >> n.x >> n.y >> n.z; temp_norm.push_back(n);
        } else if (tag == "f") {
            // faces triangles only !!
            for (int k = 0; k < 3; ++k) {
                std::string vert; ss >> vert;

                int vi = 0, ni = 0;
                size_t p1 = vert.find('/');
                if (p1 == std::string::npos) {
                    vi = std::stoi(vert);
                } else {
                    vi = std::stoi(vert.substr(0, p1));
                    size_t p2 = vert.find('/', p1 + 1);
                    if (p2 == std::string::npos) {
                        // v/vt
                    } else if (p2 == p1 + 1) {
                        // v//vn
                        ni = std::stoi(vert.substr(p2 + 1));
                    } else {
                        // v/vt/vn
                        ni = std::stoi(vert.substr(p2 + 1));
                    }
                }

                pos_idx.push_back(vi - 1);
                norm_idx.push_back(ni ? (ni - 1) : 0);
            }
        }
    }

    // build the indexed vertex set. this is necessary for the gpu to read it
    struct Key { int p, n; bool operator==(Key const& o) const { return p==o.p && n==o.n; } };
    struct KeyHash { size_t operator()(Key const& k) const noexcept { return (size_t)k.p * 1000003u + (size_t)k.n; } };

    std::unordered_map<Key, unsigned int, KeyHash> map;
    out_positions.clear(); out_normals.clear(); out_indices.clear();

    for (size_t i = 0; i < pos_idx.size(); ++i) {
        Key key{ (int)pos_idx[i], (int)norm_idx[i] };
        auto it = map.find(key);
        if (it != map.end()) {
            out_indices.push_back(it->second);
        } else {
            unsigned int newIndex = (unsigned int)out_positions.size();
            map[key] = newIndex;
            out_positions.push_back(temp_pos[key.p]);
            if (!temp_norm.empty()) out_normals.push_back(temp_norm[key.n]);
            else out_normals.push_back(glm::vec3(0.0f, 0.0f, 1.0f));
            out_indices.push_back(newIndex);
        }
    }
    return true;
}

// here we include the OpenGL shaders. vertex shader and fragment shader. vs deals with the normals and vertex while fs deals applies simple lighting and color
static const char* vs_src = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
void main() {
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* fs_src = R"GLSL(
#version 330 core
in vec3 vNormal;
out vec4 fragColor;
void main() {
    vec3 lightDir = normalize(vec3(1.0, 1.0, 0.5));
    float diff = max(dot(normalize(vNormal), lightDir), 0.0);
    vec3 base = vec3(0.6, 0.7, 0.9);
    vec3 color = base * diff + base * 0.15;
    fragColor = vec4(color, 1.0);
}
)GLSL";

// these helpers are needed to compile the shaders, create the program, and apply shaders
static GLuint compile_shader(GLenum t, const char* src){
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok){ GLint len; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len); std::string log(len, '\0'); glGetShaderInfoLog(s,len,nullptr,&log[0]); std::cerr<<log<<"\n"; glDeleteShader(s); return 0; }
    return s;
}
static GLuint link_program(GLuint vs, GLuint fs){
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if(!ok){ GLint len; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len); std::string log(len,'\0'); glGetProgramInfoLog(p,len,nullptr,&log[0]); std::cerr<<log<<"\n"; glDeleteProgram(p); return 0; }
    return p;
}

int main() {
    if (!glfwInit()) return -1;
    GLFWwindow* win = glfwCreateWindow(1280,720,"SPLENDER 0.0.0.3", nullptr, nullptr);
    if(!win){ glfwTerminate(); return -1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);  // cap to monitor refresh 
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)){ std::cerr<<"glad init failed\n"; glfwDestroyWindow(win); glfwTerminate(); return -1; }

    std::cout << "GL: " << glGetString(GL_VERSION) << " | " << glGetString(GL_RENDERER) << "\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if(!vs || !fs) return -1;
    GLuint prog = link_program(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);
    if(!prog) return -1;

    // currently the model to load is hard-coded.
    std::vector<glm::vec3> positions, normals;
    std::vector<unsigned int> indices;
    if (!load_obj_simple("assets/splender.obj", positions, normals, indices)) {
        std::cerr<<"Failed to load assets/splender.obj\n";
        return -1;
    }

    std::vector<float> verts; verts.reserve(positions.size()*6);
    for(size_t i=0;i<positions.size();++i){
        verts.push_back(positions[i].x); verts.push_back(positions[i].y); verts.push_back(positions[i].z);
        verts.push_back(normals[i].x); verts.push_back(normals[i].y); verts.push_back(normals[i].z);
    }

    GLuint vao=0, vbo=0, ebo=0;
    glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glGenBuffers(1,&ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST); // necessary for depth: objects in front occlude those behind

    GLint uMVP = glGetUniformLocation(prog, "uMVP");
    GLint uModel = glGetUniformLocation(prog, "uModel");

    // CAMERA /  ORBIT controls state
    // industry-standard: Alt + Middle = Orbit, Middle = Pan
    double lastX = 0.0, lastY = 0.0;
    bool  firstMouse = true;
    // Orbit angles (radians)
    float yaw = glm::radians(-45.0f);   // horizontal angle
    float pitch = glm::radians(25.0f);  // vertical angle, clamp to avoid flip
    float distance = 6.0f;              // distance from target
    glm::vec3 target(0.0f, 0.0f, 0.0f); // point camera orbits around

    // sensitivity
    const float orbitSpeed = 0.005f; // radians per pixel
    const float panSpeed = 0.005f;   // world units per pixel

    // clock for spinning (frame-rate independent)
    float t = 0.0f;
    auto lastTime = std::chrono::high_resolution_clock::now();
    bool paused = false;
    bool pauseKeyDown = false;


    while(!glfwWindowShouldClose(win)){
        // time update
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (!paused) {
            t += dt;
        }

        // toggle pause on SPACE press/release
        int spaceState = glfwGetKey(win, GLFW_KEY_SPACE);
        if (spaceState == GLFW_PRESS && !pauseKeyDown) {
            paused = !paused;
            pauseKeyDown = true;
        }
        if (spaceState == GLFW_RELEASE) {
            pauseKeyDown = false;
        }

        // -INPUT
        // get current mouse and button states
        double mx, my;
        glfwGetCursorPos(win, &mx, &my);
        int middleState = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_MIDDLE);
        int altState = glfwGetKey(win, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;

        if (firstMouse) {
            lastX = mx; lastY = my; firstMouse = false;
        }

        if (middleState == GLFW_PRESS) {
            // compute delta in pixels
            double dx = mx - lastX;
            double dy = my - lastY;

            if (altState) {
                // Orbit: Alt + Middle mouse
                // update yaw and pitch by mouse delta (invert X as expected)
                yaw   += float(dx) * orbitSpeed;
                pitch += float(dy) * orbitSpeed;
                // clamp pitch to avoid gimbal flip (e.g., -89..+89 degrees ETC)
                const float pitchLimit = glm::radians(89.0f);
                if (pitch > pitchLimit) pitch = pitchLimit;
                if (pitch < -pitchLimit) pitch = -pitchLimit;
            } else {
                // Pan: Middle mouse alone
                // translate target in camera's right/up plane
                // build camera basis from yaw/pitch
                float cy = cos(yaw), sy = sin(yaw);
                float cp = cos(pitch), sp = sin(pitch);

                // camera forward vector (from camera to target) in world space
                glm::vec3 forward = glm::normalize(glm::vec3(cp * cy, sp, cp * sy));
                // right vector
                glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                // up vector
                glm::vec3 up = glm::normalize(glm::cross(right, forward));

                // pan amount scaled by distance so panning feels consistent at different zooms
                glm::vec3 panOffset = float(dx) * panSpeed * right + float(dy) * panSpeed * up;
                target += panOffset * distance * 0.2f;
            }
        }

        // update last cursor pos for next frame (even if button not pressed to avoid jump)
        lastX = mx; lastY = my;

        int w,h; glfwGetFramebufferSize(win,&w,&h);
        glViewport(0,0,w,h);
        glClearColor(0.08f,0.08f,0.08f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // build camera view from orbit parameters
        // spherical -> cartesian camera position relative to target
        float cx = distance * cos(pitch) * cos(yaw);
        float cy = distance * sin(pitch);
        float cz = distance * cos(pitch) * sin(yaw);
        glm::vec3 camPos = target + glm::vec3(cx, cy, cz);

        glm::mat4 proj  = glm::perspective(glm::radians(45.0f), w>0 ? (float)w/h : 1.0f, 0.01f, 1000.0f);
        glm::mat4 view  = glm::lookAt(camPos, target, glm::vec3(0,1,0));

        // single-axis spin around Z (Z up. not y. looking at you unity)
        glm::mat4 model = glm::rotate(glm::mat4(1.0f),
                              t * 0.8f,               // spin speed (rad/sec)
                              glm::vec3(0.0f, 0.0f, 1.0f));

        // apply a small model-space translation so the object is centered at origin if needed
       

        glm::mat4 mvp   = proj * view * model;

        glUseProgram(prog);
        glUniformMatrix4fv(uMVP,   1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(uModel, 1, GL_FALSE, glm::value_ptr(model));
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
        glUseProgram(0);

        glfwSwapBuffers(win);
        glfwPollEvents();
    }


    // clean up
    glDeleteBuffers(1,&ebo);
    glDeleteBuffers(1,&vbo);
    glDeleteVertexArrays(1,&vao);
    glDeleteProgram(prog);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
