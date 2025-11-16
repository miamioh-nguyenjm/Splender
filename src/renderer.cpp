// renderer.cpp
// Renderer module for Splender: shader compilation, background, grid, UI quad, model upload & draw

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vector>
#include <string>
#include <iostream>
#include <memory>

// --- Shader helpers ---------------------------------------------------------
static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetShaderInfoLog(s, len, nullptr, &log[0]);
        std::cerr << "Shader compile error:\n" << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetProgramInfoLog(p, len, nullptr, &log[0]);
        std::cerr << "Program link error:\n" << log << "\n";
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// --- Shaders 
static const char* vs_model_src = R"GLSL(
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

static const char* fs_model_src = R"GLSL(
#version 330 core
in vec3 vNormal;
out vec4 fragColor;
void main() {
    vec3 lightDir = normalize(vec3(1.0, 1.0, 0.5));
    float diff = max(dot(normalize(vNormal), lightDir), 0.0);
    vec3 base = vec3(0.8, 0.8, 0.8);
    vec3 color = base * diff + base * 0.15;
    fragColor = vec4(color, 1.0);
}
)GLSL";

static const char* bg_vs_src = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    const vec2 verts[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 pos = verts[gl_VertexID];
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)GLSL";

static const char* bg_fs_src = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform vec3 uBottomGray;
uniform vec3 uTopBlack;
void main() {
    float t = clamp(vUV.y, 0.0, 1.0);
    vec3 col = mix(uBottomGray, uTopBlack, t);
    fragColor = vec4(col, 1.0);
}
)GLSL";

static const char* grid_vs_src = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
out vec3 vWorldPos;
void main() {
    vWorldPos = aPos;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* grid_fs_src = R"GLSL(
#version 330 core
in vec3 vWorldPos;
out vec4 fragColor;
uniform vec3 uGridColor;
uniform vec3 uAxisColorX;
uniform vec3 uAxisColorY;
uniform vec3 uAxisColorZ;
uniform float uCellSize;
uniform int uMajorEveryN;
uniform float uLineThickness;
uniform float uAxisThicknessFactor;
const float MIN_FW = 1e-4;
const float MAX_FW = 0.02;
const float LINE_PIXEL_SCALE = 1.5;
float gridLineMask(float coord, float thicknessInCells) {
    float distToLine = abs(coord - round(coord));
    float px = clamp(fwidth(coord), MIN_FW, MAX_FW);
    float thresh = max(1e-6, thicknessInCells * px * LINE_PIXEL_SCALE);
    return 1.0 - smoothstep(0.0, thresh, distToLine);
}
void main() {
    vec2 g = vWorldPos.xz / max(uCellSize, 1e-6);
    float minorX = gridLineMask(g.x, uLineThickness);
    float minorZ = gridLineMask(g.y, uLineThickness);
    float minorMask = max(minorX, minorZ);
    float majorMask = 0.0;
    if (uMajorEveryN > 1) {
        float ix = round(g.x);
        float iz = round(g.y);
        if (mod(ix, float(uMajorEveryN)) == 0.0) majorMask = max(majorMask, gridLineMask(g.x, uLineThickness * 2.0));
        if (mod(iz, float(uMajorEveryN)) == 0.0) majorMask = max(majorMask, gridLineMask(g.y, uLineThickness * 2.0));
    }
    float baseMask = max(minorMask, majorMask);
    float axisThresh = uLineThickness * uAxisThicknessFactor * 0.5;
    float axisMask_Xaxis = 1.0 - smoothstep(0.0, axisThresh, abs(vWorldPos.z));
    float axisMask_Zaxis = 1.0 - smoothstep(0.0, axisThresh, abs(vWorldPos.x));
    float originMask = 1.0 - smoothstep(0.0, uCellSize * 0.15, length(vWorldPos.xz));
    float axisDominant = max(axisMask_Xaxis, axisMask_Zaxis);
    vec3 finalColor = uGridColor;
    if (originMask > 0.001) finalColor = uAxisColorY;
    else if (axisDominant > 0.01) finalColor = (axisMask_Xaxis >= axisMask_Zaxis) ? uAxisColorX : uAxisColorZ;
    else finalColor = (majorMask > 0.5) ? uGridColor * 0.65 : uGridColor;
    float linePresence = max(baseMask, max(max(axisMask_Xaxis, axisMask_Zaxis), originMask));
    float fw = clamp(max(fwidth(g.x), fwidth(g.y)), MIN_FW, MAX_FW);
    float edge = fw * 0.75;
    float alpha = smoothstep(0.0, edge, linePresence);
    fragColor = vec4(finalColor, alpha);
}
)GLSL";

static const char* ui_vs_src = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

static const char* ui_fs_src = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform vec4 uColor;
void main() {
    fragColor = uColor;
}
)GLSL";

// ------ Renderer state ---------------------------------------------------------
struct Renderer {
    // programs
    GLuint progModel = 0;
    GLuint progBG = 0;
    GLuint progGrid = 0;
    GLuint progUI = 0;

    // VAOs/VBOs
    GLuint vaoModel = 0;
    GLuint vboModel = 0;
    GLuint eboModel = 0;
    GLsizei modelIndexCount = 0;

    GLuint vaoBG = 0;
    GLuint vaoGrid = 0;
    GLuint vboGrid = 0;
    GLuint eboGrid = 0;

    GLuint vaoUI = 0;
    GLuint vboUI = 0;

    // uniform locations (cached)
    GLint uMVP_model = -1;
    GLint uModel_model = -1;

    GLint uBottomGray_bg = -1;
    GLint uTopBlack_bg = -1;

    GLint uMVP_grid = -1;

    GLint uColor_ui = -1;

    Renderer() = default;

    bool init() {
        // compile/link programs
        GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_model_src);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_model_src);
        if (!vs || !fs) return false;
        progModel = link_program(vs, fs);
        glDeleteShader(vs); glDeleteShader(fs);
        if (!progModel) return false;
        uMVP_model = glGetUniformLocation(progModel, "uMVP");
        uModel_model = glGetUniformLocation(progModel, "uModel");

        // background
        GLuint bvs = compile_shader(GL_VERTEX_SHADER, bg_vs_src);
        GLuint bfs = compile_shader(GL_FRAGMENT_SHADER, bg_fs_src);
        if (bvs && bfs) {
            progBG = link_program(bvs, bfs);
        }
        if (bvs) glDeleteShader(bvs);
        if (bfs) glDeleteShader(bfs);
        if (progBG) {
            uBottomGray_bg = glGetUniformLocation(progBG, "uBottomGray");
            uTopBlack_bg = glGetUniformLocation(progBG, "uTopBlack");
            glGenVertexArrays(1, &vaoBG); // dummy VAO for fullscreen triangle
        }

        // grid
        GLuint gvs = compile_shader(GL_VERTEX_SHADER, grid_vs_src);
        GLuint gfs = compile_shader(GL_FRAGMENT_SHADER, grid_fs_src);
        if (gvs && gfs) {
            progGrid = link_program(gvs, gfs);
        }
        if (gvs) glDeleteShader(gvs);
        if (gfs) glDeleteShader(gfs);
        if (progGrid) {
            uMVP_grid = glGetUniformLocation(progGrid, "uMVP");
            // create grid mesh (large quad on XZ plane)
            const float R = 10.0f;
            float gridVerts[] = { -R, 0.0f, -R,  R, 0.0f, -R,  R, 0.0f, R,  -R, 0.0f, R };
            unsigned int gridIdx[] = { 0,1,2, 0,2,3 };
            glGenVertexArrays(1, &vaoGrid);
            glGenBuffers(1, &vboGrid);
            glGenBuffers(1, &eboGrid);
            glBindVertexArray(vaoGrid);
            glBindBuffer(GL_ARRAY_BUFFER, vboGrid);
            glBufferData(GL_ARRAY_BUFFER, sizeof(gridVerts), gridVerts, GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboGrid);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(gridIdx), gridIdx, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
            glBindVertexArray(0);
        }

        // UI quad
        GLuint uvs = compile_shader(GL_VERTEX_SHADER, ui_vs_src);
        GLuint ufs = compile_shader(GL_FRAGMENT_SHADER, ui_fs_src);
        if (uvs && ufs) {
            progUI = link_program(uvs, ufs);
        }
        if (uvs) glDeleteShader(uvs);
        if (ufs) glDeleteShader(ufs);
        if (progUI) {
            uColor_ui = glGetUniformLocation(progUI, "uColor");
            // fullscreen quad VBO that drawRectNDC uses with scissor
            float uiVerts[] = {
                -1.0f, -1.0f, 0.0f, 0.0f,
                 1.0f, -1.0f, 1.0f, 0.0f,
                 1.0f,  1.0f, 1.0f, 1.0f,
                -1.0f, -1.0f, 0.0f, 0.0f,
                 1.0f,  1.0f, 1.0f, 1.0f,
                -1.0f,  1.0f, 0.0f, 1.0f
            };
            glGenVertexArrays(1, &vaoUI);
            glGenBuffers(1, &vboUI);
            glBindVertexArray(vaoUI);
            glBindBuffer(GL_ARRAY_BUFFER, vboUI);
            glBufferData(GL_ARRAY_BUFFER, sizeof(uiVerts), uiVerts, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
            glBindVertexArray(0);
        }

        return true;
    }

    // Upload interleaved model data (positions + normals) and indices
    void uploadModel(const std::vector<float>& interleavedVerts, const std::vector<unsigned int>& indices) {
        // delete existing
        if (eboModel) { glDeleteBuffers(1, &eboModel); eboModel = 0; }
        if (vboModel) { glDeleteBuffers(1, &vboModel); vboModel = 0; }
        if (vaoModel) { glDeleteVertexArrays(1, &vaoModel); vaoModel = 0; }

        glGenVertexArrays(1, &vaoModel);
        glGenBuffers(1, &vboModel);
        glGenBuffers(1, &eboModel);

        glBindVertexArray(vaoModel);
        glBindBuffer(GL_ARRAY_BUFFER, vboModel);
        glBufferData(GL_ARRAY_BUFFER, interleavedVerts.size() * sizeof(float), interleavedVerts.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboModel);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        // layout: vec3 pos, vec3 normal (stride = 6 floats)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));

        glBindVertexArray(0);
        modelIndexCount = (GLsizei)indices.size();
    }

    void drawBackground() {
        if (!progBG) return;
        GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLint prevDepthMask = 1;
        glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        glUseProgram(progBG);
        if (uBottomGray_bg >= 0) glUniform3f(uBottomGray_bg, 0.45f, 0.45f, 0.45f);
        if (uTopBlack_bg >= 0) glUniform3f(uTopBlack_bg, 0.0f, 0.0f, 0.0f);

        glBindVertexArray(vaoBG);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(prevDepthMask);
        glUseProgram(0);
    }

    void drawGrid(const glm::mat4& mvp) {
        if (!progGrid) return;
        // depth test on, depth writes off
        GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLint prevDepthMask = 1;
        glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        glUseProgram(progGrid);
        if (uMVP_grid >= 0) glUniformMatrix4fv(uMVP_grid, 1, GL_FALSE, glm::value_ptr(mvp));
        // appearance uniforms (fallback values)
        GLint loc = glGetUniformLocation(progGrid, "uGridColor");
        if (loc >= 0) glUniform3f(loc, 0.6f, 0.6f, 0.6f);
        loc = glGetUniformLocation(progGrid, "uAxisColorX"); if (loc >= 0) glUniform3f(loc, 0.79f, 0.24f, 0.28f);
        loc = glGetUniformLocation(progGrid, "uAxisColorY"); if (loc >= 0) glUniform3f(loc, 0.52f, 0.84f, 0.40f);
        loc = glGetUniformLocation(progGrid, "uAxisColorZ"); if (loc >= 0) glUniform3f(loc, 0.47f, 0.79f, 0.24f);
        loc = glGetUniformLocation(progGrid, "uCellSize"); if (loc >= 0) glUniform1f(loc, 1.0f);
        loc = glGetUniformLocation(progGrid, "uMajorEveryN"); if (loc >= 0) glUniform1i(loc, 10);
        loc = glGetUniformLocation(progGrid, "uLineThickness"); if (loc >= 0) glUniform1f(loc, 0.5f);
        loc = glGetUniformLocation(progGrid, "uAxisThicknessFactor"); if (loc >= 0) glUniform1f(loc, 0.1f);

        glBindVertexArray(vaoGrid);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        glDepthMask(prevDepthMask);
        if (!wasDepthTest) glDisable(GL_DEPTH_TEST);
        glUseProgram(0);
    }

    void drawModel(const glm::mat4& mvp, const glm::mat4& modelMat) {
        if (!progModel || !vaoModel || modelIndexCount == 0) return;
        glUseProgram(progModel);
        if (uMVP_model >= 0) glUniformMatrix4fv(uMVP_model, 1, GL_FALSE, glm::value_ptr(mvp));
        if (uModel_model >= 0) glUniformMatrix4fv(uModel_model, 1, GL_FALSE, glm::value_ptr(modelMat));
        glBindVertexArray(vaoModel);
        glDrawElements(GL_TRIANGLES, modelIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    // Draw a filled rectangle in NDC using scissor to restrict area (caller must set scissor).
    void drawRectNDC(float x0, float y0, float x1, float y1, const glm::vec4& color) {
        if (!progUI || !vaoUI) return;
        glUseProgram(progUI);
        if (uColor_ui >= 0) glUniform4f(uColor_ui, color.r, color.g, color.b, color.a);
        glBindVertexArray(vaoUI);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    void cleanup() {
        if (progModel) glDeleteProgram(progModel); progModel = 0;
        if (progBG) glDeleteProgram(progBG); progBG = 0;
        if (progGrid) glDeleteProgram(progGrid); progGrid = 0;
        if (progUI) glDeleteProgram(progUI); progUI = 0;

        if (vaoModel) glDeleteVertexArrays(1, &vaoModel); vaoModel = 0;
        if (vboModel) glDeleteBuffers(1, &vboModel); vboModel = 0;
        if (eboModel) glDeleteBuffers(1, &eboModel); eboModel = 0;

        if (vaoBG) glDeleteVertexArrays(1, &vaoBG); vaoBG = 0;
        if (vaoGrid) glDeleteVertexArrays(1, &vaoGrid); vaoGrid = 0;
        if (vboGrid) glDeleteBuffers(1, &vboGrid); vboGrid = 0;
        if (eboGrid) glDeleteBuffers(1, &eboGrid); eboGrid = 0;

        if (vaoUI) glDeleteVertexArrays(1, &vaoUI); vaoUI = 0;
        if (vboUI) glDeleteBuffers(1, &vboUI); vboUI = 0;
    }
};

static Renderer g_renderer;

// Simple API wrappers
bool renderer_init() { return g_renderer.init(); }
void renderer_cleanup() { g_renderer.cleanup(); }
void renderer_upload_model(const std::vector<float>& verts6f, const std::vector<unsigned int>& idx) {
    g_renderer.uploadModel(verts6f, idx);
}
void renderer_draw_background() { g_renderer.drawBackground(); }
void renderer_draw_grid(const glm::mat4& mvp) { g_renderer.drawGrid(mvp); }
void renderer_draw_model(const glm::mat4& mvp, const glm::mat4& modelMat) { g_renderer.drawModel(mvp, modelMat); }
void renderer_draw_rect_ndc(float x0, float y0, float x1, float y1, const glm::vec4& color) {
    // Caller should set scissor with window coords before calling this function
    g_renderer.drawRectNDC(x0, y0, x1, y1, color);
}
