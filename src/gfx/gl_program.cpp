// ps5-homebrew-ui - GLSL program compilation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/gl_program.hpp"

#include <cstdint>

#include "platform/ps5/system.hpp"

namespace hui::gfx
{

namespace
{

const char *g_glsl_prefix = "#version 460 core\n";

} // namespace

void set_glsl_prefix(const char *prefix)
{
    g_glsl_prefix = prefix;
}

namespace
{

GLuint compile(const char *label, GLenum stage, const char *source)
{
    const char *sources[] = {g_glsl_prefix, source};
    sys::log("[HUI] shader %s stage=%u create start", label, static_cast<unsigned>(stage));
    GLuint shader = glCreateShader(stage);
    sys::log("[HUI] shader %s create done id=%u", label, static_cast<unsigned>(shader));
    sys::log("[HUI] shader %s source start", label);
    glShaderSource(shader, 2, sources, nullptr);
    sys::log("[HUI] shader %s source done", label);
    sys::log("[HUI] shader %s compile start", label);
    glCompileShader(shader);
    sys::log("[HUI] shader %s compile done", label);
    GLint ok = GL_FALSE;
    sys::log("[HUI] shader %s status start", label);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    sys::log("[HUI] shader %s status done ok=%d", label, ok == GL_TRUE ? 1 : 0);
    if (ok == GL_TRUE)
        return shader;
    char info[512] = {};
    glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
    sys::log("[HUI] shader %s (%s) failed: %s", label,
             stage == GL_VERTEX_SHADER ? "vertex" : "fragment", info);
    glDeleteShader(shader);
    return 0;
}

} // namespace

GLuint build_program(const char *label, const char *vertex_source, const char *fragment_source)
{
    // Start-up is mostly this: each program is logged with its time.
    sys::log("[HUI] program %s start", label);
    const std::int64_t started = sys::monotonic_us();
    GLuint vertex = compile(label, GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = vertex != 0 ? compile(label, GL_FRAGMENT_SHADER, fragment_source) : 0;
    if (fragment == 0)
    {
        if (vertex != 0)
            glDeleteShader(vertex);
        return 0;
    }
    sys::log("[HUI] program %s create start", label);
    GLuint program = glCreateProgram();
    sys::log("[HUI] program %s create done id=%u", label, static_cast<unsigned>(program));
    sys::log("[HUI] program %s attach start", label);
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    sys::log("[HUI] program %s attach done", label);
    sys::log("[HUI] program %s link start", label);
    glLinkProgram(program);
    sys::log("[HUI] program %s link done", label);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    sys::log("[HUI] program %s shaders deleted", label);
    GLint ok = GL_FALSE;
    sys::log("[HUI] program %s status start", label);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    sys::log("[HUI] program %s status done ok=%d", label, ok == GL_TRUE ? 1 : 0);
    sys::log("[HUI] program %s built in %lld ms", label,
             static_cast<long long>((sys::monotonic_us() - started) / 1000));
    if (ok == GL_TRUE)
        return program;
    char info[512] = {};
    glGetProgramInfoLog(program, sizeof(info), nullptr, info);
    sys::log("[HUI] program %s link failed: %s", label, info);
    glDeleteProgram(program);
    return 0;
}

} // namespace hui::gfx
