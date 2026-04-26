#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace geode::prelude;

namespace cb {
    constexpr int kToggleMenuTag = 0x43535F80;
    constexpr int kControllerTag = 0x43535F81;

    constexpr char const* kTextureShaderKey = "realgares.colorblind-support/texture-filter-v3";
    constexpr char const* kColorShaderKey   = "realgares.colorblind-support/color-filter-v3";

    constexpr char const* kTextureVertPath = "colorblind_texture.vert";
    constexpr char const* kTextureFragPath = "colorblind_texture.frag";
    constexpr char const* kColorVertPath   = "colorblind_color.vert";
    constexpr char const* kColorFragPath   = "colorblind_color.frag";

    static bool s_forceRefresh = true;
    static CCScene* s_lastScene = nullptr;

    /* Stores original shader for nodes that this mod actually touched. */
    static std::unordered_map<CCNode*, CCGLProgram*> s_originalPrograms;

    static bool enabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    static std::string mode() {
        return Mod::get()->getSettingValue<std::string>("mode");
    }

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

    static std::optional<std::string> readResourceText(char const* relativePath) {
        auto path = Mod::get()->getResourcesDir() / relativePath;

        std::ifstream file(path, std::ios::in | std::ios::binary);

        if (!file.is_open()) {
            log::error("Failed to open shader resource: {}", path.string());
            return std::nullopt;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();

        auto source = buffer.str();

        if (source.empty()) {
            log::error("Shader resource is empty: {}", path.string());
            return std::nullopt;
        }

        return source;
    }

    static CCGLProgram* defaultTextureShader() {
        return CCShaderCache::sharedShaderCache()->programForKey(
            kCCShader_PositionTextureColor
        );
    }

    static CCGLProgram* defaultColorShader() {
        return CCShaderCache::sharedShaderCache()->programForKey(
            kCCShader_PositionColor
        );
    }

    static CCGLProgram* createProgram(
        char const* key,
        char const* vertexPath,
        char const* fragmentPath,
        bool usesTextureCoords
    ) {
        auto cache = CCShaderCache::sharedShaderCache();

        if (auto existing = cache->programForKey(key)) {
            return existing;
        }

        auto vertexSource = readResourceText(vertexPath);
        auto fragmentSource = readResourceText(fragmentPath);

        if (!vertexSource || !fragmentSource) {
            log::error("Failed to load shader pair: {} / {}", vertexPath, fragmentPath);
            return nullptr;
        }

        auto program = new CCGLProgram();

        if (!program) {
            return nullptr;
        }

        if (!program->initWithVertexShaderByteArray(
            vertexSource->c_str(),
            fragmentSource->c_str()
        )) {
            log::error("Failed to initialise shader program: {}", key);
            CC_SAFE_DELETE(program);
            return nullptr;
        }

        program->addAttribute(kCCAttributeNamePosition, kCCVertexAttrib_Position);
        program->addAttribute(kCCAttributeNameColor, kCCVertexAttrib_Color);

        if (usesTextureCoords) {
            program->addAttribute(kCCAttributeNameTexCoord, kCCVertexAttrib_TexCoords);
        }

        if (!program->link()) {
            log::error("Failed to link shader program: {}", key);
            CC_SAFE_DELETE(program);
            return nullptr;
        }

        program->updateUniforms();

        cache->addProgram(program, key);
        program->release();

        return cache->programForKey(key);
    }

    static CCGLProgram* textureProgram() {
        return createProgram(
            kTextureShaderKey,
            kTextureVertPath,
            kTextureFragPath,
            true
        );
    }

    static CCGLProgram* colorProgram() {
        return createProgram(
            kColorShaderKey,
            kColorVertPath,
            kColorFragPath,
            false
        );
    }

    static void updateProgramUniforms(CCGLProgram* program) {
        if (!program) {
            return;
        }

        program->use();

        auto strengthLocation = program->getUniformLocationForName("u_strength");
        auto modeLocation = program->getUniformLocationForName("u_mode");

        program->setUniformLocationWith1f(strengthLocation, strength());
        program->setUniformLocationWith1f(modeLocation, modeIndex());
    }

    static void updateShaderUniforms() {
        updateProgramUniforms(textureProgram());
        updateProgramUniforms(colorProgram());
    }

    template <class T>
    static void applyProgramToNode(
        T* renderNode,
        CCGLProgram* filterProgram,
        CCGLProgram* fallbackProgram
    ) {
        if (!renderNode || !filterProgram) {
            return;
        }

        auto node = static_cast<CCNode*>(renderNode);

        if (node->getTag() == kToggleMenuTag || node->getTag() == kControllerTag) {
            return;
        }

        auto current = renderNode->getShaderProgram();
        auto stored = s_originalPrograms.find(node);

        /* Only stock/default shaders are safe to replace. */
        auto hasCustomShader =
            current &&
            current != fallbackProgram &&
            current != filterProgram;

        if (enabled()) {
            if (hasCustomShader) {
                if (stored != s_originalPrograms.end()) {
                    s_originalPrograms.erase(stored);
                }

                return;
            }

            if (current == filterProgram) {
                return;
            }

            s_originalPrograms[node] = current ? current : fallbackProgram;
            renderNode->setShaderProgram(filterProgram);
            return;
        }

        /**
         * Disabled path.
         *
         * Restore only if node still uses this mod's shader. If another mod
         * changed it after this mod touched it, leave that shader alone.
         */
        if (stored != s_originalPrograms.end()) {
            if (current == filterProgram) {
                renderNode->setShaderProgram(
                    stored->second ? stored->second : fallbackProgram
                );
            }

            s_originalPrograms.erase(stored);
            return;
        }

        /* Hot reload. */
        if (current == filterProgram) {
            renderNode->setShaderProgram(fallbackProgram);
        }
    }

    static void applyShaderTree(CCNode* node) {
        if (!node) {
            return;
        }

        if (node->getTag() == kToggleMenuTag || node->getTag() == kControllerTag) {
            return;
        }

        /* Apply the shader to the batch owner. */
        if (auto batch = typeinfo_cast<CCSpriteBatchNode*>(node)) {
            applyProgramToNode(batch, textureProgram(), defaultTextureShader());
            return;
        }

        if (auto sprite = typeinfo_cast<CCSprite*>(node)) {
            applyProgramToNode(sprite, textureProgram(), defaultTextureShader());
        }
        else if (auto colorLayer = typeinfo_cast<CCLayerColor*>(node)) {
            applyProgramToNode(colorLayer, colorProgram(), defaultColorShader());
        }

        for (auto child : node->getChildrenExt<CCNode*>()) {
            applyShaderTree(child);
        }
    }

    static void refreshRunningScene() {
        auto scene = CCDirector::sharedDirector()->getRunningScene();

        if (!scene) {
            return;
        }

        if (scene != s_lastScene) {
            s_lastScene = scene;
            s_originalPrograms.clear();
            s_forceRefresh = true;
        }

        updateShaderUniforms();
        applyShaderTree(scene);
    }

    static void requestRefresh() {
        s_forceRefresh = true;
    }

    class GlobalController final : public CCNode {
    public:
        float m_timer = 0.f;

        bool m_hasState = false;
        bool m_lastEnabled = false;
        float m_lastStrength = -1.f;
        float m_lastMode = -1.f;

        static GlobalController* create() {
            auto ret = new GlobalController();

            if (ret && ret->init()) {
                ret->autorelease();
                return ret;
            }

            CC_SAFE_DELETE(ret);
            return nullptr;
        }

        bool init() override {
            if (!CCNode::init()) {
                return false;
            }

            this->scheduleUpdate();
            return true;
        }

        void update(float dt) override {
            m_timer += dt;

            auto nowEnabled = enabled();
            auto nowStrength = strength();
            auto nowMode = modeIndex();

            auto settingsChanged =
                !m_hasState ||
                m_lastEnabled != nowEnabled ||
                std::abs(m_lastStrength - nowStrength) > 0.0001f ||
                std::abs(m_lastMode - nowMode) > 0.0001f;

            if (s_forceRefresh || settingsChanged || m_timer >= 0.12f) {
                refreshRunningScene();

                s_forceRefresh = false;
                m_timer = 0.f;

                m_hasState = true;
                m_lastEnabled = nowEnabled;
                m_lastStrength = nowStrength;
                m_lastMode = nowMode;
            }
        }
    };

    static void installGlobalController() {
        auto director = CCDirector::sharedDirector();

        if (!director) {
            return;
        }

        auto host = director->getNotificationNode();

        if (!host) {
            host = CCNode::create();
            director->setNotificationNode(host);
        }

        if (host->getChildByTag(kControllerTag)) {
            return;
        }

        auto controller = GlobalController::create();

        if (!controller) {
            return;
        }

        controller->setTag(kControllerTag);
        host->addChild(controller);
    }
}

class $modify(CBMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        cb::installGlobalController();

        this->addColorblindToggleButton();

        /* `MenuLayer::init` can run before every sprite has settled. */
        cb::requestRefresh();

        return true;
    }

    void addColorblindToggleButton() {
        if (auto oldMenu = this->getChildByTag(cb::kToggleMenuTag)) {
            oldMenu->removeFromParentAndCleanup(true);
        }

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        auto menu = CCMenu::create();
        menu->setTag(cb::kToggleMenuTag);
        menu->setPosition(ccp(winSize.width - 42.f, winSize.height - 32.f));

        auto sprite = ButtonSprite::create("CB");
        sprite->setScale(0.66f);

        sprite->setColor(
            cb::enabled()
                ? ccc3(105, 255, 85)
                : ccc3(255, 95, 80)
        );

        auto button = CCMenuItemSpriteExtra::create(
            sprite,
            this,
            menu_selector(CBMenuLayer::onColorblindToggle)
        );

        button->setScale(0.82f);
        menu->addChild(button);

        this->addChild(menu, 9999);
    }

    void onColorblindToggle(CCObject*) {
        auto next = !cb::enabled();

        Mod::get()->setSettingValue<bool>("enabled", next);

        this->addColorblindToggleButton();

        cb::requestRefresh();
        cb::refreshRunningScene();
    }
};