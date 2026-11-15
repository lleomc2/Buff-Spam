// Spam Buff - mod de Geode para Geometry Dash (Android 2.2081 / Geode 5.11.2).
//
// Arquitectura: el juego entrega cada toque a GJBaseGameLayer::handleButton.
// Spam Buff se mete ahi: deja pasar los toques normales sin tocarlos y, cuando
// detecta una rafaga de spam, toma el control con un motor de estados
// (SpamEngine.hpp) que mantiene el boton (Hold) o genera pulsos regulares (Pulso).
// El motor avanza una vez por paso de fisica desde GJBaseGameLayer::processCommands,
// asi cada cambio de estado cae justo en un paso, sin depender de los FPS.
//
// Auto-spam: una tecla (Geode keybind) o el boton AUTO en pantalla encienden un
// generador de clics a intervalo fijo (ms), tambien servido por el mismo motor.

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
    bool showAutoButton = true;
    bool autoStopOnDeath = true;
    bool keyHold = false;
    bool injecting = false;
    bool autoWanted = false;  // lo que pidio el jugador (tecla / boton)
    bool autoShown = false;   // lo que muestra el boton en pantalla
    int indicatorState = 0;   // 0 apagado, 1 SPAM, 2 AUTO
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
    c.autoIntervalMs = static_cast<double>(mod->getSettingValue<int64_t>("auto-interval-ms"));
    c.autoPressMs = static_cast<double>(mod->getSettingValue<int64_t>("auto-press-ms"));
    r.cfg = c;
    r.showIndicator = mod->getSettingValue<bool>("show-indicator");
    r.showAutoButton = mod->getSettingValue<bool>("show-auto-button");
    r.autoStopOnDeath = mod->getSettingValue<bool>("auto-stop-on-death");
    r.keyHold = mod->getSettingValue<std::string>("auto-key-mode") == "Mantener";
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

// 0 = oculto, 1 = "SPAM" (deteccion automatica), 2 = "AUTO" (tecla / boton)
void setIndicator(int state) {
    auto& r = rt();
    if (!r.showIndicator) state = 0;
    if (r.indicatorState == state) return;
    r.indicatorState = state;
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_uiLayer) return;
    auto* node = pl->m_uiLayer->getChildByID("spam-buff-indicator"_spr);
    if (auto* label = typeinfo_cast<CCLabelBMFont*>(node)) {
        if (state != 0) label->setString(state == 2 ? "AUTO" : "SPAM");
        label->setVisible(state != 0);
    }
}

// Deja el boton en pantalla igual que el estado pedido.
void syncAutoButton() {
    auto& r = rt();
    if (r.autoShown == r.autoWanted) return;
    r.autoShown = r.autoWanted;
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_uiLayer) return;
    auto* menu = pl->m_uiLayer->getChildByID("spam-buff-auto-menu"_spr);
    if (!menu) return;
    auto* node = menu->getChildByID("spam-buff-auto-button"_spr);
    if (auto* toggler = typeinfo_cast<CCMenuItemToggler*>(node)) {
        toggler->toggle(r.autoWanted);
    }
}

void stopAuto() {
    rt().autoWanted = false;
    syncAutoButton();
}

// Si el mod dejaba el boton apretado (hold / auto-spam) y el dedo ya no esta, lo soltamos.
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
        setIndicator(r.engine.ch[0].autoActive() ? 2 : (r.engine.anySpam() ? 1 : 0));
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& r = rt();
        if (isMainPlayLayer(this)) {
            if ((r.calls++ % 30u) == 0u) refreshConfig();
            if (!r.cfg.enabled) r.autoWanted = false;

            double dtMs = static_cast<double>(dt) * 1000.0;
            r.engine.now += dtMs;

            // Auto-spam: solo el jugador 1 (igual que un toque normal).
            auto& main = r.engine.ch[0];
            if (main.autoActive() != r.autoWanted) {
                spambuff::Emitted out;
                main.setAuto(r.autoWanted, r.cfg, out);
                this->sbDeliver(out, true);
            }

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
            syncAutoButton();
            setIndicator(main.autoActive() ? 2 : (r.engine.anySpam() ? 1 : 0));
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(SBPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        refreshConfig();
        auto& r = rt();
        r.engine.reset();
        r.autoWanted = false;
        r.autoShown = false;
        r.indicatorState = 0;
        if (!m_uiLayer) return;

        auto win = CCDirector::get()->getWinSize();

        auto* label = CCLabelBMFont::create("SPAM", "bigFont.fnt");
        label->setID("spam-buff-indicator"_spr);
        label->setScale(0.35f);
        label->setOpacity(170);
        label->setAnchorPoint({0.f, 1.f});
        label->setVisible(false);
        label->setPosition({8.f, win.height - 6.f});
        m_uiLayer->addChild(label, 100);

        if (r.showAutoButton) {
            auto* offSpr = ButtonSprite::create("AUTO", "goldFont.fnt", "GJ_button_06.png", 0.55f);
            auto* onSpr = ButtonSprite::create("AUTO", "goldFont.fnt", "GJ_button_01.png", 0.55f);
            auto* button = CCMenuItemToggler::create(
                offSpr, onSpr, this, menu_selector(SBPlayLayer::onSpamAutoButton)
            );
            button->setID("spam-buff-auto-button"_spr);
            button->toggle(false);
            button->setPosition({38.f, win.height - 40.f});

            auto* menu = CCMenu::create();
            menu->setID("spam-buff-auto-menu"_spr);
            menu->setPosition({0.f, 0.f});
            menu->addChild(button);
            m_uiLayer->addChild(menu, 100);
        }
    }

    // Boton AUTO en pantalla: enciende / apaga el auto-spam.
    void onSpamAutoButton(CCObject*) {
        auto& r = rt();
        refreshConfig();
        r.autoWanted = r.cfg.enabled && !r.autoWanted;
        // El toggler ya cambio su dibujo solo; syncAutoButton lo corrige si hace falta.
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& r = rt();
        r.engine.reset();
        if (r.autoStopOnDeath) stopAuto();
        setIndicator(0);
    }

    void pauseGame(bool unfocused) {
        releaseLatched(this);
        PlayLayer::pauseGame(unfocused);
        rt().engine.reset();
        stopAuto();
        setIndicator(0);
    }

    void onQuit() {
        rt().engine.reset();
        rt().autoWanted = false;
        PlayLayer::onQuit();
    }

    void levelComplete() {
        rt().engine.reset();
        stopAuto();
        setIndicator(0);
        PlayLayer::levelComplete();
    }
};

// Boton rapido ON/OFF del mod en el menu de pausa (abajo a la izquierda).
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

// Tecla de auto-spam (ajuste "auto-spam-key"). Alternar: una pulsacion enciende/apaga.
// Mantener: hace spam solo mientras la tecla esta apretada. Requiere teclado
// (en el celu usa el boton AUTO en pantalla).
$execute {
    geode::listenForKeybindSettingPresses(
        "auto-spam-key",
        [](geode::Keybind const&, bool down, bool repeat, double) -> bool {
            if (repeat || !PlayLayer::get()) return false;
            refreshConfig();
            auto& r = rt();
            if (!r.cfg.enabled) return false;
            if (r.keyHold) {
                r.autoWanted = down;
            } else if (down) {
                r.autoWanted = !r.autoWanted;
            }
            return false;
        }
    );
}
