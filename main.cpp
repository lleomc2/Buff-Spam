// Spam Buff - mod de Geode para Geometry Dash (Android 2.2081 / Geode 5.11.2).
//
// Arquitectura: el juego entrega cada toque a GJBaseGameLayer::handleButton.
// Spam Buff se mete ahi: deja pasar los toques normales sin tocarlos y, cuando
// detecta una rafaga de spam, toma el control con un motor de estados
// (SpamEngine.hpp) que mantiene el boton (Hold) o genera pulsos regulares (Pulso).
// El motor avanza una vez por paso de fisica desde GJBaseGameLayer::processCommands,
// asi cada cambio de estado cae justo en un paso, sin depender de los FPS.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "SpamEngine.hpp"

using namespace geode::prelude;

namespace {

struct Runtime {
    spambuff::Engine engine;
    spambuff::Config cfg;
    bool showIndicator = true;
    bool injecting = false;
    bool indicatorOn = false;
    unsigned calls = 0;
};

Runtime& rt() {
    static Runtime r;
    return r;
}

void refreshConfig() {
    auto* mod = Mod::get();
    auto& r = rt();
    spambuff::Config c;
    c.enabled = mod->getSettingValue<bool>("enabled");
    auto mode = mod->getSettingValue<std::string>("mode");
    c.mode = mode == "Hold" ? spambuff::Mode::Hold
           : (mode == "Pulso" ? spambuff::Mode::Pulse : spambuff::Mode::Auto);
    c.triggerCps = static_cast<double>(mod->getSettingValue<int64_t>("trigger-cps"));
    c.minTaps = static_cast<int>(mod->getSettingValue<int64_t>("min-taps"));
    c.speedAuto = mod->getSettingValue<bool>("speed-auto");
    c.spamCps = static_cast<double>(mod->getSettingValue<int64_t>("spam-cps"));
    c.precision = static_cast<int>(mod->getSettingValue<int64_t>("precision"));
    c.dutyPercent = static_cast<int>(mod->getSettingValue<int64_t>("pulse-duty"));
    c.protectTaps = mod->getSettingValue<bool>("protect-taps");
    r.cfg = c;
    r.showIndicator = mod->getSettingValue<bool>("show-indicator");
}

// Solo actuamos en el PlayLayer de una partida (no en el editor ni en otros contextos).
bool isMainPlayLayer(GJBaseGameLayer* layer) {
    auto* pl = PlayLayer::get();
    return pl != nullptr && static_cast<GJBaseGameLayer*>(pl) == layer;
}

// Cubo (y platformer): mantener presionado = saltos continuos. El resto de modos
// (nave, ufo, bola, wave, robot, spider, swing) se comportan mejor con pulsos.
bool cubeLike(GJBaseGameLayer* layer, bool isPlayer1) {
    PlayerObject* p = isPlayer1 ? layer->m_player1 : layer->m_player2;
    if (!p) return true;
    return !(p->m_isShip || p->m_isBird || p->m_isBall || p->m_isDart ||
             p->m_isRobot || p->m_isSpider || p->m_isSwing);
}

void setIndicator(bool on) {
    auto& r = rt();
    on = on && r.showIndicator;
    if (r.indicatorOn == on) return;
    r.indicatorOn = on;
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_uiLayer) return;
    if (auto* label = pl->m_uiLayer->getChildByID("spam-buff-indicator"_spr)) {
        label->setVisible(on);
    }
}

// Si el mod dejaba el boton apretado (hold) y el dedo ya no esta, lo soltamos.
void releaseLatched(GJBaseGameLayer* layer) {
    auto& r = rt();
    for (int i = 0; i < 2; ++i) {
        if (r.engine.ch[i].wantsRelease()) {
            r.injecting = true;
            layer->handleButton(false, 1, i == 0);
            r.injecting = false;
        }
    }
}

} // namespace

class $modify(SBBaseGameLayer, GJBaseGameLayer) {
    // Entrega al juego los eventos que decidio el motor (marcados como "inyectados"
    // para que nuestro propio hook no los vuelva a procesar).
    void sbDeliver(spambuff::Emitted const& out, bool isPlayer1) {
        auto& r = rt();
        for (int i = 0; i < out.n; ++i) {
            r.injecting = true;
            GJBaseGameLayer::handleButton(out.down[i], 1, isPlayer1);
            r.injecting = false;
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& r = rt();
        if (r.injecting || button != 1 || !r.cfg.enabled || !isMainPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        spambuff::Emitted out;
        r.engine.channel(isPlayer1).onReal(
            down, r.engine.now, cubeLike(this, isPlayer1), r.cfg, out
        );
        this->sbDeliver(out, isPlayer1);
        setIndicator(r.engine.anySpam());
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& r = rt();
        if (isMainPlayLayer(this)) {
            if ((r.calls++ % 30u) == 0u) refreshConfig();
            double dtMs = static_cast<double>(dt) * 1000.0;
            r.engine.now += dtMs;
            for (int i = 0; i < 2; ++i) {
                spambuff::Emitted out;
                auto& ch = r.engine.ch[i];
                if (r.cfg.enabled) {
                    ch.tick(r.engine.now, dtMs, r.cfg, out);
                } else {
                    ch.shutdown(out);
                }
                this->sbDeliver(out, i == 0);
            }
            setIndicator(r.engine.anySpam());
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(SBPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        refreshConfig();
        rt().engine.reset();
        rt().indicatorOn = false;

        auto* label = CCLabelBMFont::create("SPAM", "bigFont.fnt");
        label->setID("spam-buff-indicator"_spr);
        label->setScale(0.35f);
        label->setOpacity(170);
        label->setAnchorPoint({0.f, 1.f});
        label->setVisible(false);
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({8.f, win.height - 6.f});
        if (m_uiLayer) m_uiLayer->addChild(label, 100);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        rt().engine.reset();
        setIndicator(false);
    }

    void pauseGame(bool unfocused) {
        releaseLatched(this);
        PlayLayer::pauseGame(unfocused);
        rt().engine.reset();
        setIndicator(false);
    }

    void onQuit() {
        rt().engine.reset();
        PlayLayer::onQuit();
    }

    void levelComplete() {
        rt().engine.reset();
        setIndicator(false);
        PlayLayer::levelComplete();
    }
};

// Boton rapido ON/OFF en el menu de pausa (abajo a la izquierda).
class $modify(SBPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        refreshConfig();

        auto* offSpr = ButtonSprite::create("Spam OFF", "goldFont.fnt", "GJ_button_06.png", 0.6f);
        auto* onSpr = ButtonSprite::create("Spam ON", "goldFont.fnt", "GJ_button_01.png", 0.6f);
        auto* toggler = CCMenuItemToggler::create(
            offSpr, onSpr, this, menu_selector(SBPauseLayer::onSpamToggle)
        );
        toggler->toggle(rt().cfg.enabled);
        toggler->setID("spam-buff-toggle"_spr);
        toggler->setPosition({62.f, 26.f});

        auto* menu = CCMenu::create();
        menu->setID("spam-buff-menu"_spr);
        menu->setPosition({0.f, 0.f});
        menu->addChild(toggler);
        this->addChild(menu, 20);
    }

    void onSpamToggle(CCObject*) {
        auto* mod = Mod::get();
        bool now = !mod->getSettingValue<bool>("enabled");
        mod->setSettingValue<bool>("enabled", now);
        refreshConfig();
    }
};
