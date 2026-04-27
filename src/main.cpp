#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemToggler.hpp>

#include <Geode/ui/Notification.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace geode::prelude;

namespace cb {
    constexpr int kControllerTag = 0x43535FA0;

    constexpr char const* kPostVertexPath = "colorblind_post.vert";
    constexpr char const* kPostFragPath = "colorblind_post.frag";

    static CCGLProgram* s_program;

    /* Cached copy target for finished backbuffer. */
    static GLuint s_frameTexture = 0;
    static int s_frameWidth = 0;
    static int s_frameHeight = 0;

    /* Set when OpenGL context may have changed. */
    static bool s_graphicsDirty = true;

    /* Stops repeated err spam when shader files are missing or invalid. */
    static bool s_programBuildFailed = false;

    /* Get "enabled" setting. */
    static bool enabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    /* Get "mode" setting. */
    static std::string mode() {
        return Mod::get()->getSettingValue<std::string>("mode");
    }

    /* Get "strength" setting. */
    static float strength() {
        return static_cast<float>(std::clamp(
            Mod::get()->getSettingValue<double>("strength"),
            0.0,
            1.0
        ));
    }

    static float modeIndex() {
        auto selected = mode();

        if (selected == "Protanopia") {
            return 1.f;
        }

        if (selected == "Tritanopia") {
            return 2.f;
        }

        if (selected == "Achromatopsia") {
            return 3.f;
        }

        return 0.f;
    }

    static Result<std::string> readShaderSource(char const* relativePath) {
        auto path = Mod::get()->getResourcesDir() / relativePath;
        return utils::file::readString(path);
    }

    static void deleteFrameTexture() {
        if (s_frameTexture != 0) {
            glDeleteTextures(1, &s_frameTexture);
            s_frameTexture = 0;
        }

        s_frameWidth = 0;
        s_frameHeight = 0;
    }

    static void discardGraphicsHandles() {
        s_program = nullptr;
        s_frameTexture = 0;
        s_frameWidth = 0;
        s_frameHeight = 0;
        s_programBuildFailed = false;
    }

    static void markGraphicsDirty() {
        s_graphicsDirty = true;
    }

    static CCGLProgram* createPostProgram() {
        auto vertexResult = readShaderSource(kPostVertexPath);
        auto fragmentResult = readShaderSource(kPostFragPath);

        if (vertexResult.isErr()) {
            log::error(
                "Failed to read vertex shader '{}': {}",
                kPostVertexPath,
                vertexResult.unwrapErr()
            );
            return nullptr;
        }

        if (fragmentResult.isErr()) {
            log::error(
                "Failed to read fragment shader '{}': {}",
                kPostFragPath,
                fragmentResult.unwrapErr()
            );
            return nullptr;
        }

        auto vertexSource = vertexResult.unwrap();
        auto fragmentSource = fragmentResult.unwrap();

        auto program = new CCGLProgram();

        if (!program) {
            log::error("Failed to allocate Colorblind Support shader program");
            return nullptr;
        }

        if (!program->initWithVertexShaderByteArray(
            vertexSource.c_str(),
            fragmentSource.c_str()
        )) {
            log::error("Failed to initialise Colorblind Support post-process shader");
            CC_SAFE_DELETE(program);
            return nullptr;
        }

        /* Fullscreen pass only needs pos + texture coords. */
        program->addAttribute(kCCAttributeNamePosition, kCCVertexAttrib_Position);
        program->addAttribute(kCCAttributeNameTexCoord, kCCVertexAttrib_TexCoords);

        if (!program->link()) {
            log::error("Failed to link Colorblind Support post-process shader");
            CC_SAFE_DELETE(program);
            return nullptr;
        }

        program->updateUniforms();

        return program;
    }

    static CCGLProgram* postProgram() {
        if (s_graphicsDirty) {
            discardGraphicsHandles();
            s_graphicsDirty = false;
        }

        if (s_program) {
            return s_program;
        }

        if (s_programBuildFailed) {
            return nullptr;
        }

        s_program = createPostProgram();

        if (!s_program) {
            s_programBuildFailed = true;
            return nullptr;
        }

        return s_program;
    }

    static bool ensureFrameTexture(int width, int height) {
        if (width <= 0 || height <= 0) {
            return false;
        }

        if (
            s_frameTexture != 0 &&
            s_frameWidth == width &&
            s_frameHeight == height
        ) {
            return true;
        }

        /* Only runs during normal rendering when ctx is alive. */
        deleteFrameTexture();

        glGenTextures(1, &s_frameTexture);

        if (s_frameTexture == 0) {
            log::error("Failed to create Colorblind Support frame texture");
            return false;
        }

        s_frameWidth = width;
        s_frameHeight = height;

        GLint oldActiveTexture = 0;
        GLint oldTexture0 = 0;

        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);

        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);

        glBindTexture(GL_TEXTURE_2D, s_frameTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        /* Clamp avoids sampling garbage outside copied frame when quad lands on edge of viewport. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr
        );

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));

        return true;
    }

    static void updateUniforms(CCGLProgram* program) {
        if (!program) {
            return;
        }

        program->use();

        auto textureLocation = program->getUniformLocationForName("u_texture");
        auto strengthLocation = program->getUniformLocationForName("u_strength");
        auto modeLocation = program->getUniformLocationForName("u_mode");

        program->setUniformLocationWith1i(textureLocation, 0);
        program->setUniformLocationWith1f(strengthLocation, strength());
        program->setUniformLocationWith1f(modeLocation, modeIndex());
    }

    static void drawFullscreenQuad() {
        struct Vertex {
            GLfloat x;
            GLfloat y;
            GLfloat u;
            GLfloat v;
        };

        /* Tex coords match `glCopyTexSubImage2D` lower-left origin. */
        static constexpr Vertex vertices[] = {
            { -1.f, -1.f, 0.f, 0.f },
            {  1.f, -1.f, 1.f, 0.f },
            { -1.f,  1.f, 0.f, 1.f },
            {  1.f,  1.f, 1.f, 1.f }
        };

        glEnableVertexAttribArray(kCCVertexAttrib_Position);
        glEnableVertexAttribArray(kCCVertexAttrib_TexCoords);

        glVertexAttribPointer(
            kCCVertexAttrib_Position,
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(Vertex),
            &vertices[0].x
        );

        glVertexAttribPointer(
            kCCVertexAttrib_TexCoords,
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(Vertex),
            &vertices[0].u
        );

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    static void applyPostProcess() {
        auto amount = strength();

        if (!enabled() || amount <= 0.001f) {
            return;
        }

        auto program = postProgram();

        if (!program) {
            return;
        }

        GLint viewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, viewport);

        auto width = viewport[2];
        auto height = viewport[3];

        if (!ensureFrameTexture(width, height)) {
            return;
        }

        /* Preserve bits of GL state most likely to matter if another hook renders after this one. */
        GLint oldProgram = 0;
        GLint oldActiveTexture = 0;
        GLint oldTexture0 = 0;
        GLint oldArrayBuffer = 0;
        GLint oldViewport[4] = { 0, 0, 0, 0 };

        GLboolean oldBlend = glIsEnabled(GL_BLEND);
        GLboolean oldDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLboolean oldScissorTest = glIsEnabled(GL_SCISSOR_TEST);
        GLboolean oldCullFace = glIsEnabled(GL_CULL_FACE);

        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
        glGetIntegerv(GL_VIEWPORT, oldViewport);

        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);

        /* Copy frame after GD, particles, menus, custom shaders and mod UI have already rendered. */
        glBindTexture(GL_TEXTURE_2D, s_frameTexture);
        glCopyTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            viewport[0],
            viewport[1],
            width,
            height
        );

        /* Fullscreen replacement pass. */
        if (oldBlend) {
            glDisable(GL_BLEND);
        }

        if (oldDepthTest) {
            glDisable(GL_DEPTH_TEST);
        }

        if (oldScissorTest) {
            glDisable(GL_SCISSOR_TEST);
        }

        if (oldCullFace) {
            glDisable(GL_CULL_FACE);
        }

        glViewport(viewport[0], viewport[1], width, height);

        program->use();
        updateUniforms(program);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_frameTexture);

        drawFullscreenQuad();

        /* Put GL state back. */
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldArrayBuffer));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));

        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        glUseProgram(static_cast<GLuint>(oldProgram));

        if (oldBlend) {
            glEnable(GL_BLEND);
        }
        else {
            glDisable(GL_BLEND);
        }

        if (oldDepthTest) {
            glEnable(GL_DEPTH_TEST);
        }
        else {
            glDisable(GL_DEPTH_TEST);
        }

        if (oldScissorTest) {
            glEnable(GL_SCISSOR_TEST);
        }
        else {
            glDisable(GL_SCISSOR_TEST);
        }

        if (oldCullFace) {
            glEnable(GL_CULL_FACE);
        }
        else {
            glDisable(GL_CULL_FACE);
        }

        glViewport(
            oldViewport[0],
            oldViewport[1],
            oldViewport[2],
            oldViewport[3]
        );
    }

    static CCMenu* findMenuLayerButtonHost(MenuLayer* layer) {
        if (!layer) {
            return nullptr;
        }

        if (auto menu = typeinfo_cast<CCMenu*>(layer->getChildByID("right-side-menu"))) {
            return menu;
        }
        if (auto menu = typeinfo_cast<CCMenu*>(layer->getChildByID("bottom-menu"))) {
            return menu;
        }

        return nullptr;
    }

    static ButtonSprite* createToggleSprite(bool active) {
        auto sprite = ButtonSprite::create("CB");

        if (!sprite) {
            return nullptr;
        }

        sprite->setScale(0.66f);

        sprite->setColor(
            active
                ? ccc3(105, 255, 85)
                : ccc3(255, 95, 90)
        );

        return sprite;
    }
}

$on_mod(Loaded) {
    cb::markGraphicsDirty();
}

$on_game(TexturesLoaded) {
    cb::markGraphicsDirty();
}

class $modify(CBMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        auto menu = cb::findMenuLayerButtonHost(this);

        if (!menu) {
            log::warn("Could not find MenuLayer menu for Colorblind Support toggle");
            return true;
        }

        if (!menu->getChildByID("toggle-button"_spr)) {
            auto toggler = CCMenuItemToggler::create(
                cb::createToggleSprite(false),
                cb::createToggleSprite(true),
                this,
                menu_selector(CBMenuLayer::onColorblindToggle)
            );

            if (toggler) {
                toggler->setID("toggle-button"_spr);
                toggler->setScale(0.82f);

                /* Sync initial visual state with saved setting. */
                toggler->toggle(cb::enabled());

                menu->addChild(toggler);

                /* Let existing menu place new item. */
                if (menu->getLayout()) {
                    menu->updateLayout();
                }
            }
        }

        cb::markGraphicsDirty();
        return true;
    }

    void onColorblindToggle(CCObject*) {
        auto next = !cb::enabled();

        Mod::get()->setSettingValue<bool>("enabled", next);

        Notification::create(
            next ? "Colorblind Support enabled" : "Colorblind Support disabled",
            next ? NotificationIcon::Success : NotificationIcon::Info,
            0.8f
        )->show();
    }
};

class $modify(CBCCEGLView, CCEGLView) {
    void swapBuffers() {
        /* Apply filter AFTER frame has drawn but BEFORE backbuffer is presented. */
        cb::applyPostProcess();

        CCEGLView::swapBuffers();
    }
};