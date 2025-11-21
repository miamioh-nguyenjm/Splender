#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    // initialize renderer resources that require a valid GL context
    bool init();

    // compile/link builtin shader programs and create simple VAOs/VBOs
    bool createBuiltinPrograms();

    // set per-program uniforms used by app.cpp
    void setModelMVP(const glm::mat4& mvp);
    void setModelMatrix(const glm::mat4& model);
    void setLightDirection(const glm::vec3& dir);
    void setLightIntensity(float intensity);
    void setLightColor(const glm::vec3& color);
    void setEnableShadows(bool enable);
    void setForceWire(bool force);
    void setWireColor(const glm::vec3& color);

    // grid/background/ui helpers
    void drawBackground();
    void drawGrid(const glm::mat4& mvpGrid);
    void drawUIQuad();

    // program/vao accessors needed by app.cpp for model drawing / EBO binding
    GLuint modelProgram() const { return prog_; }
    GLuint bgProgram() const { return bg_prog_; }
    GLuint gridProgram() const { return grid_prog_; }
    GLuint uiProgram() const { return ui_prog_; }
    GLuint bgVao() const { return bg_vao_; }
    GLuint gridVao() const { return grid_vao_; }
    GLuint uiVao() const { return ui_vao_; }
    GLint model_uMVP_loc() const { return uMVP_; }
    GLint model_uModel_loc() const { return uModel_; }

    // cleanup GL-owned resources (call with valid context)
    void shutdownCleanup();

private:
    // shader/program helpers (internal)
    static GLuint compile_shader(GLenum t, const char* src);
    static GLuint link_program(GLuint vs, GLuint fs);

    // shader sources (kept priv
    static const char* vs_src_;
    static const char* fs_src_;
    static const char* bg_vs_src_;
    static const char* bg_fs_src_;
    static const char* grid_vs_src_;
    static const char* grid_fs_src_;
    static const char* ui_vs_src_;
    static const char* ui_fs_src_;

    // programs + VAOs/VBOs owned by renderer
    GLuint prog_ = 0;
    GLuint bg_prog_ = 0;
    GLuint grid_prog_ = 0;
    GLuint ui_prog_ = 0;

    GLuint bg_vao_ = 0;
    GLuint grid_vao_ = 0;
    GLuint grid_vbo_ = 0;
    GLuint grid_ebo_ = 0;
    GLuint ui_vao_ = 0;
    GLuint ui_vbo_ = 0;

    // cached model program uniform locations
    GLint uMVP_ = -1;
    GLint uModel_ = -1;
    GLint uForceWire_ = -1;
    GLint uWireColor_ = -1;
    GLint uLightDir_ = -1;
    GLint uLightIntensity_ = -1;
    GLint uLightColor_ = -1;
    GLint uEnableShadows_ = -1;
};
