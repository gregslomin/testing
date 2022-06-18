/*
 * Copyright (c) 2020 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/opt.h"
#include "internal.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

#include <GLFW/glfw3.h>

typedef struct EENFisheyeContext {
    const AVClass *class;
    GLuint        program;
    GLuint        frame_tex;
    GLFWwindow    *window;
    GLuint        pos_buf;
} EENFisheyeContext;

static const float position[12] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f};

static const GLchar *v_shader_source =
        "attribute vec2 position;\n"
        "varying vec2 texCoord;\n"
        "void main(void) {\n"
        "  gl_Position = vec4(position, 0, 1);\n"
        "  texCoord = position;\n"
        "}\n";

static const GLchar *f_shader_source =
        "uniform sampler2D tex;\n"
        "varying vec2 texCoord;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(tex, texCoord * 0.5 + 0.5);\n"
        "}\n";

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type) {
    GLuint shader = glCreateShader(type);
    if (!shader || !glIsShader(shader)) {
        return 0;
    }
    glShaderSource(shader, 1, &shader_source, 0);
    glCompileShader(shader);
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    return status == GL_TRUE ? shader : 0;
}



static void tex_setup(AVFilterLink *inlink) {
    AVFilterContext     *ctx = inlink->dst;
    EENFisheyeContext *gs = ctx->priv;

    glGenTextures(1, &gs->frame_tex);
    glActiveTexture(GL_TEXTURE0);

    glBindTexture(GL_TEXTURE_2D, gs->frame_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, inlink->w, inlink->h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    glUniform1i(glGetUniformLocation(gs->program, "tex"), 0);
}

static int build_program(AVFilterContext *ctx) {
    GLuint v_shader, f_shader;
    EENFisheyeContext *gs = ctx->priv;

    if (!((v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER)) &&
          (f_shader = build_shader(ctx, f_shader_source, GL_FRAGMENT_SHADER)))) {
        return -1;
    }

    gs->program = glCreateProgram();
    glAttachShader(gs->program, v_shader);
    glAttachShader(gs->program, f_shader);
    glLinkProgram(gs->program);

    GLint status;
    glGetProgramiv(gs->program, GL_LINK_STATUS, &status);
    return status == GL_TRUE ? 0 : -1;
}
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM



static void vbo_setup(EENFisheyeContext *gs) {
    glGenBuffers(1, &gs->pos_buf);
    glBindBuffer(GL_ARRAY_BUFFER, gs->pos_buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

    GLint loc = glGetAttribLocation(gs->program, "position");
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
}

#define OFFSET(x) offsetof(EENFisheyeContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption eenfisheye_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(eenfisheye);

#define f(n, m) (dst[(n) * width + (m)])


static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_NONE
};

static av_cold void uninit(AVFilterContext *ctx)
{
    EENFisheyeContext *gs = ctx->priv;
    glDeleteTextures(1, &gs->frame_tex);
    glDeleteProgram(gs->program);
    glDeleteBuffers(1, &gs->pos_buf);
    glfwDestroyWindow(gs->window);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext     *ctx = inlink->dst;
    EENFisheyeContext *gs = ctx->priv;

    glfwWindowHint(GLFW_VISIBLE, 0);
    gs->window = glfwCreateWindow(inlink->w, inlink->h, "", NULL, NULL);

    glfwMakeContextCurrent(gs->window);

#ifndef __APPLE__
    glewExperimental = GL_TRUE;
    glewInit();
#endif

    glViewport(0, 0, inlink->w, inlink->h);

    int ret;
    if((ret = build_program(ctx)) < 0) {
        return ret;
    }

    glUseProgram(gs->program);
    vbo_setup(gs);
    tex_setup(inlink);
    return 0;
}


static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx     = inlink->dst;
    AVFilterLink    *outlink = ctx->outputs[0];

    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, inlink->w, inlink->h, 0, GL_RGB, GL_UNSIGNED_BYTE, in->data[0]);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(0, 0, outlink->w, outlink->h, GL_RGB, GL_UNSIGNED_BYTE, (GLvoid *)out->data[0]);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad eenfisheye_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad eenfisheye_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

const AVFilter ff_vf_eenfisheye = {
    .name          = "eenfisheye",
    .description   = NULL_IF_CONFIG_SMALL("Apply EEN Fisheye filter."),
    .priv_size     = sizeof(EENFisheyeContext),
    .priv_class    = &eenfisheye_class,
    .uninit        = uninit,
    FILTER_INPUTS(eenfisheye_inputs),
    FILTER_OUTPUTS(eenfisheye_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .process_command = ff_filter_process_command,
};
