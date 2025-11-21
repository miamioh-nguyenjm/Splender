//renderer.cpp
// shaders and rendering...
#include "renderer.h"
#include <iostream>
#include <string>
#include <glm/gtc/type_ptr.hpp>

const char* Renderer::vs_src_ = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
void main(){ vNormal = mat3(transpose(inverse(uModel)))*aNormal; gl_Position = uMVP * vec4(aPos,1.0); }
)GLSL";

const char* Renderer::fs_src_ = R"GLSL(
#version 330 core
in vec3 vNormal;
out vec4 fragColor;

uniform vec3 uLightDir;
uniform float uLightIntensity;
uniform vec3 uLightColor;
uniform bool uForceWire;
uniform vec3 uWireColor;
uniform bool uEnableShadows;

void main(){
    if (uForceWire) { fragColor = vec4(uWireColor, 1.0); return; }

    vec3 L = normalize(uLightDir);
    float NdotL = max(dot(normalize(vNormal), L), 0.0);

    vec3 base = vec3(0.8);
    vec3 lit = base * (NdotL * uLightIntensity) + base * 0.15;

    if (uEnableShadows) {
        float shadowFactor = mix(0.5, 1.0, smoothstep(0.0, 0.6, NdotL));
        lit *= shadowFactor;
    }

    fragColor = vec4(lit * uLightColor, 1.0);
}
)GLSL";

const char* Renderer::bg_vs_src_ = R"GLSL(
#version 330 core
out vec2 vUV;
void main(){ const vec2 V[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3)); vec2 p = V[gl_VertexID]; vUV = p*0.5+0.5; gl_Position = vec4(p,0,1); }
)GLSL";

const char* Renderer::bg_fs_src_ = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform vec3 uBottomGray; uniform vec3 uTopBlack;
void main(){ float t=clamp(vUV.y,0.0,1.0); fragColor = vec4(mix(uBottomGray,uTopBlack,t),1.0); }
)GLSL";

const char* Renderer::grid_vs_src_ = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos; uniform mat4 uMVP; out vec3 vWorldPos;
void main(){ vWorldPos = aPos; gl_Position = uMVP * vec4(aPos,1.0); }
)GLSL";

const char* Renderer::grid_fs_src_ = R"GLSL(
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

const char* Renderer::ui_vs_src_ = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos; layout(location=1) in vec2 aUV; out vec2 vUV;
void main(){ vUV=aUV; gl_Position=vec4(aPos,0,1); }
)GLSL";

const char* Renderer::ui_fs_src_ = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor; uniform vec4 uColor;
void main(){ fragColor = uColor; }
)GLSL";

// --- shader helper implementations -------------
GLuint Renderer::compile_shader(GLenum t, const char* src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len ? len : 1, '\0');
        glGetShaderInfoLog(s, len, nullptr, &log[0]);
        std::cerr << "Shader compile error:\n" << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint Renderer::link_program(GLuint vs, GLuint fs) {
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len ? len : 1, '\0');
        glGetProgramInfoLog(p, len, nullptr, &log[0]);
        std::cerr << "Program link error:\n" << log << "\n";
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

Renderer::~Renderer() {
}

bool Renderer::init() {
    return true;
}

bool Renderer::createBuiltinPrograms() {
    prog_ = link_program(compile_shader(GL_VERTEX_SHADER, vs_src_), compile_shader(GL_FRAGMENT_SHADER, fs_src_));
    if (!prog_) return false;
    uMVP_ = glGetUniformLocation(prog_, "uMVP");
    uModel_ = glGetUniformLocation(prog_, "uModel");
    uForceWire_ = glGetUniformLocation(prog_, "uForceWire");
    uWireColor_ = glGetUniformLocation(prog_, "uWireColor");
    uLightDir_ = glGetUniformLocation(prog_, "uLightDir");
    uLightIntensity_ = glGetUniformLocation(prog_, "uLightIntensity");
    uLightColor_ = glGetUniformLocation(prog_, "uLightColor");
    uEnableShadows_ = glGetUniformLocation(prog_, "uEnableShadows");
    if (uEnableShadows_ >= 0) {
        glUseProgram(prog_);
        glUniform1i(uEnableShadows_, 0);
        glUseProgram(0);
    }

    bg_prog_ = link_program(compile_shader(GL_VERTEX_SHADER, bg_vs_src_), compile_shader(GL_FRAGMENT_SHADER, bg_fs_src_));
    if (bg_prog_) { glGenVertexArrays(1, &bg_vao_); }

    grid_prog_ = link_program(compile_shader(GL_VERTEX_SHADER, grid_vs_src_), compile_shader(GL_FRAGMENT_SHADER, grid_fs_src_));
    if (grid_prog_) {
        glGenVertexArrays(1, &grid_vao_);
        glGenBuffers(1, &grid_vbo_);
        glGenBuffers(1, &grid_ebo_);
        const float R = 10.0f;
        float gridVerts[] = { -R,0,-R, R,0,-R, R,0,R, -R,0,R };
        unsigned int gridIdx[] = {0,1,2, 0,2,3};
        glBindVertexArray(grid_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, grid_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(gridVerts), gridVerts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, grid_ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(gridIdx), gridIdx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);
        glBindVertexArray(0);
    }

    // UI quad
    glGenVertexArrays(1, &ui_vao_);
    glGenBuffers(1, &ui_vbo_);
    glBindVertexArray(ui_vao_);
    float uiVerts[] = {
        -1.0f,-1.0f, 0.0f,0.0f,
         1.0f,-1.0f, 1.0f,0.0f,
         1.0f, 1.0f, 1.0f,1.0f,
        -1.0f,-1.0f, 0.0f,0.0f,
         1.0f, 1.0f, 1.0f,1.0f,
        -1.0f, 1.0f, 0.0f,1.0f
    };
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(uiVerts), uiVerts, GL_STATIC_DRAW);
    ui_prog_ = link_program(compile_shader(GL_VERTEX_SHADER, ui_vs_src_), compile_shader(GL_FRAGMENT_SHADER, ui_fs_src_));
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glBindVertexArray(0);

    return true;
}

// uniform setters
void Renderer::setModelMVP(const glm::mat4& mvp) {
    if (prog_ && uMVP_ >= 0) {
        glUseProgram(prog_);
        glUniformMatrix4fv(uMVP_, 1, GL_FALSE, glm::value_ptr(mvp));
        glUseProgram(0);
    }
}
void Renderer::setModelMatrix(const glm::mat4& model) {
    if (prog_ && uModel_ >= 0) {
        glUseProgram(prog_);
        glUniformMatrix4fv(uModel_, 1, GL_FALSE, glm::value_ptr(model));
        glUseProgram(0);
    }
}
void Renderer::setLightDirection(const glm::vec3& dir) {
    if (prog_ && uLightDir_ >= 0) {
        glUseProgram(prog_);
        glm::vec3 d = glm::normalize(dir);
        glUniform3f(uLightDir_, d.x, d.y, d.z);
        glUseProgram(0);
    }
}
void Renderer::setLightIntensity(float intensity) {
    if (prog_ && uLightIntensity_ >= 0) {
        glUseProgram(prog_);
        glUniform1f(uLightIntensity_, intensity);
        glUseProgram(0);
    }
}
void Renderer::setLightColor(const glm::vec3& color) {
    if (prog_ && uLightColor_ >= 0) {
        glUseProgram(prog_);
        glUniform3f(uLightColor_, color.r, color.g, color.b);
        glUseProgram(0);
    }
}
void Renderer::setEnableShadows(bool enable) {
    if (prog_ && uEnableShadows_ >= 0) {
        glUseProgram(prog_);
        glUniform1i(uEnableShadows_, enable ? 1 : 0);
        glUseProgram(0);
    }
}
void Renderer::setForceWire(bool force) {
    if (prog_ && uForceWire_ >= 0) {
        glUseProgram(prog_);
        glUniform1i(uForceWire_, force ? 1 : 0);
        glUseProgram(0);
    }
}
void Renderer::setWireColor(const glm::vec3& color) {
    if (prog_ && uWireColor_ >= 0) {
        glUseProgram(prog_);
        glUniform3f(uWireColor_, color.r, color.g, color.b);
        glUseProgram(0);
    }
}

// Draw helpers
void Renderer::drawBackground() {
    if (!bg_prog_) return;
    glUseProgram(bg_prog_);
    GLint locBottom = glGetUniformLocation(bg_prog_, "uBottomGray");
    GLint locTop = glGetUniformLocation(bg_prog_, "uTopBlack");
    if (locBottom >= 0) glUniform3f(locBottom, 0.45f, 0.45f, 0.45f);
    if (locTop >= 0) glUniform3f(locTop, 0.0f, 0.0f, 0.0f);

    GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevDepthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    if (wasDepthTest) glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glBindVertexArray(bg_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    if (wasDepthTest) glEnable(GL_DEPTH_TEST);
    glDepthMask(prevDepthMask);
    glUseProgram(0);
}

void Renderer::drawGrid(const glm::mat4& mvpGrid) {
    if (!grid_prog_) return;
    glUseProgram(grid_prog_);
    GLint grid_uMVP = glGetUniformLocation(grid_prog_, "uMVP");
    if (grid_uMVP >= 0) glUniformMatrix4fv(grid_uMVP, 1, GL_FALSE, glm::value_ptr(mvpGrid));
    GLint loc = glGetUniformLocation(grid_prog_, "uGridColor"); if (loc>=0) glUniform3f(loc, 0.6f,0.6f,0.6f);
    loc = glGetUniformLocation(grid_prog_, "uAxisColorX"); if (loc>=0) glUniform3f(loc, 0.79f,0.24f,0.28f);
    loc = glGetUniformLocation(grid_prog_, "uAxisColorY"); if (loc>=0) glUniform3f(loc, 0.45f,0.83f,0.28f);
    loc = glGetUniformLocation(grid_prog_, "uAxisColorZ"); if (loc>=0) glUniform3f(loc, 0.20f,0.60f,0.92f);
    loc = glGetUniformLocation(grid_prog_, "uCellSize"); if (loc>=0) glUniform1f(loc, 1.0f);
    loc = glGetUniformLocation(grid_prog_, "uMajorEveryN"); if (loc>=0) glUniform1i(loc, 10);
    loc = glGetUniformLocation(grid_prog_, "uLineThickness"); if (loc>=0) glUniform1f(loc, 0.5f);
    loc = glGetUniformLocation(grid_prog_, "uAxisThicknessFactor"); if (loc>=0) glUniform1f(loc, 0.1f);

    GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevDepthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glBindVertexArray(grid_vao_);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glDepthMask(prevDepthMask);
    if (!wasDepthTest) glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
}

void Renderer::drawUIQuad() {
    if (!ui_prog_) return;
    glUseProgram(ui_prog_);
    glBindVertexArray(ui_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void Renderer::shutdownCleanup() {
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
    if (bg_prog_) { glDeleteProgram(bg_prog_); bg_prog_ = 0; }
    if (grid_prog_) { glDeleteProgram(grid_prog_); grid_prog_ = 0; }
    if (ui_prog_) { glDeleteProgram(ui_prog_); ui_prog_ = 0; }

    if (ui_vbo_) { glDeleteBuffers(1, &ui_vbo_); ui_vbo_ = 0; }
    if (ui_vao_) { glDeleteVertexArrays(1, &ui_vao_); ui_vao_ = 0; }

    if (grid_ebo_) { glDeleteBuffers(1, &grid_ebo_); grid_ebo_ = 0; }
    if (grid_vbo_) { glDeleteBuffers(1, &grid_vbo_); grid_vbo_ = 0; }
    if (grid_vao_) { glDeleteVertexArrays(1, &grid_vao_); grid_vao_ = 0; }

    if (bg_vao_) { glDeleteVertexArrays(1, &bg_vao_); bg_vao_ = 0; }
}
