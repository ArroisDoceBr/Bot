// Auto Learn Bot - robô que aprende a fase por tentativa e erro (v2).
// Funciona no modo prática (não vale como progresso oficial).
//
// Como funciona:
//  1. A fase é dividida em trechos curtos.
//  2. Em cada trecho o bot testa poucos cliques (toques curtos ou longos).
//  3. Se morre, volta pro checkpoint do bot e testa outro padrão.
//  4. Se passa o trecho (e está em lugar seguro), salva os cliques e cria
//     um novo checkpoint (guardado pelo próprio bot, sem marcar na tela).
//  5. Se falha demais, volta vários trechos pra trás (cada vez mais) e
//     tenta outra saída, pra não ficar preso em checkpoint ruim.
//  6. Ao completar a fase, entra em modo REPLAY e repete tudo sozinho.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <random>
#include <vector>

using namespace geode::prelude;

namespace {
    enum class Mode { Off, Learning, Replay };

    struct Event {
        int frame;
        bool down;
    };

    struct Segment {
        int startFrame = 0;
        bool startHolding = false;
        size_t committedCount = 0; // tamanho da gravação quando o trecho começou
        int failures = 0;
    };

    constexpr int SEG_LEN = 120;  // tamanho de cada trecho
    constexpr int MAX_FAILS = 30; // falhas antes de voltar trechos

    Mode g_mode = Mode::Off;
    int g_frame = 0;
    bool g_holding = false;
    std::vector<Event> g_macro; // cliques confirmados (a fase aprendida)
    std::vector<Event> g_trial; // cliques da tentativa atual
    size_t g_trialIdx = 0;
    size_t g_replayIdx = 0;
    std::vector<Segment> g_segs;
    std::vector<CheckpointObject*> g_cps; // checkpoints do bot (um por trecho, menos o primeiro)
    int g_backDepth = 1;  // quantos trechos voltar quando travar
    int g_bestFrame = 0;  // ponto mais longe que já chegou
    std::mt19937 g_rng{std::random_device{}()};

    int rnd(int a, int b) {
        return std::uniform_int_distribution<int>(a, b)(g_rng);
    }

    bool isFlying(PlayerObject* p) {
        return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
    }

    void clearCheckpoints() {
        for (auto cp : g_cps) cp->release();
        g_cps.clear();
    }

    // Gera poucos cliques para o trecho atual.
    void makeTrial() {
        g_trial.clear();
        g_trialIdx = 0;
        if (g_segs.empty()) return;

        auto const& seg = g_segs.back();
        auto pl = PlayLayer::get();
        bool flying = pl && pl->m_player1 && isFlying(pl->m_player1);

        int from = seg.startFrame + 1;
        int to = seg.startFrame + SEG_LEN;
        int t = from;

        // se o botão já estava apertado, solta em algum momento
        if (seg.startHolding) {
            t = from + (rnd(0, 99) < 60 ? rnd(0, 20) : rnd(0, SEG_LEN - 2));
            g_trial.push_back({t, false});
            t += 1;
        }

        // às vezes não faz nada (deixa o jogo correr)
        if (!flying && rnd(0, 99) < 15) return;

        int taps;
        if (flying) {
            taps = rnd(1, 5);
        } else {
            int r = rnd(0, 99);
            taps = r < 70 ? 1 : (r < 95 ? 2 : 3);
        }

        for (int i = 0; i < taps && t < to; ++i) {
            int start = rnd(t, to - 1);
            int dur;
            if (flying) dur = rnd(3, 30);
            else dur = rnd(0, 99) < 70 ? rnd(2, 12) : rnd(20, 100);
            g_trial.push_back({start, true});
            g_trial.push_back({start + dur, false});
            t = start + dur + 1;
        }
    }

    // Aperta ou solta o botão (só se o estado mudou).
    void applyInput(GJBaseGameLayer* gl, bool down) {
        if (g_holding == down) return;
        g_holding = down;
        gl->handleButton(down, 1, true);
    }

    // Executa os cliques planejados para o tick atual.
    void tickInput(GJBaseGameLayer* gl) {
        if (g_mode == Mode::Learning) {
            if (g_segs.empty()) return;
            auto const& seg = g_segs.back();
            if (g_frame == seg.startFrame) {
                // ao (re)começar um trecho, o botão começa solto
                g_holding = false;
                applyInput(gl, seg.startHolding);
            }
            while (g_trialIdx < g_trial.size() && g_trial[g_trialIdx].frame <= g_frame) {
                applyInput(gl, g_trial[g_trialIdx].down);
                g_trialIdx++;
            }
        } else if (g_mode == Mode::Replay) {
            while (g_replayIdx < g_macro.size() && g_macro[g_replayIdx].frame <= g_frame) {
                applyInput(gl, g_macro[g_replayIdx].down);
                g_replayIdx++;
            }
        }
    }

    // Salva na gravação só os cliques que já aconteceram neste trecho.
    void saveExecutedInputs() {
        auto const seg = g_segs.back();
        if (seg.startHolding) g_macro.push_back({seg.startFrame, true});
        for (size_t i = 0; i < g_trialIdx && i < g_trial.size(); ++i) {
            g_macro.push_back(g_trial[i]);
        }
    }

    // O trecho foi vencido: salva os cliques, cria checkpoint e começa o próximo.
    void commitSegment(PlayLayer* pl) {
        saveExecutedInputs();

        Segment next;
        next.startFrame = g_frame;
        next.startHolding = g_holding;
        next.committedCount = g_macro.size();
        g_segs.push_back(next);

        auto cp = pl->createCheckpoint();
        cp->retain();
        g_cps.push_back(cp);

        if (g_frame > g_bestFrame) {
            g_bestFrame = g_frame;
            g_backDepth = 1;
        }

        makeTrial();
    }

    bool isActivePlayLayer(GJBaseGameLayer* self) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == self;
    }
}

class $modify(BotGJBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        bool active = g_mode != Mode::Off && isActivePlayLayer(this);
        if (active) tickInput(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (active) g_frame++;
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        ButtonSprite* label = nullptr;
    };

    void refreshLabel() {
        if (!m_fields->label) return;
        const char* txt = "BOT";
        if (g_mode == Mode::Learning) txt = "APRENDENDO";
        if (g_mode == Mode::Replay) txt = "REPLAY";
        m_fields->label->setString(txt);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        g_mode = Mode::Off;
        g_segs.clear();
        g_macro.clear();
        clearCheckpoints();

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});

        auto spr = ButtonSprite::create("BOT");
        spr->setScale(0.6f);
        m_fields->label = spr;

        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BotPlayLayer::onBotButton));
        auto win = CCDirector::get()->getWinSize();
        btn->setPosition({70.f, win.height - 30.f});
        menu->addChild(btn);

        m_uiLayer->addChild(menu, 100);
    }

    void onBotButton(CCObject*) {
        if (g_mode == Mode::Off) {
            if (!m_isPracticeMode) this->togglePracticeMode(true);

            g_segs.clear();
            g_macro.clear();
            clearCheckpoints();
            g_frame = 0;
            g_holding = false;
            g_bestFrame = 0;
            g_backDepth = 1;
            g_mode = Mode::Learning;
            this->resetLevelFromStart();
        } else {
            g_mode = Mode::Off;
        }
        this->refreshLabel();
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        if (g_mode == Mode::Replay) {
            g_frame = 0;
            g_holding = false;
            g_replayIdx = 0;
            return;
        }
        if (g_mode != Mode::Learning) return;

        if (g_segs.empty()) {
            g_macro.clear();
            clearCheckpoints();
            g_bestFrame = 0;
            g_backDepth = 1;
            g_segs.push_back(Segment{});
        } else if (g_frame > g_segs.back().startFrame) {
            g_segs.back().failures++;
        }

        // falhou demais: volta alguns trechos (cada vez mais) e tenta outra saída
        if (g_segs.back().failures >= MAX_FAILS && g_segs.size() > 1) {
            int pops = std::min<int>(g_backDepth, (int)g_segs.size() - 1);
            for (int i = 0; i < pops; ++i) {
                g_segs.pop_back();
                if (!g_cps.empty()) {
                    g_cps.back()->release();
                    g_cps.pop_back();
                }
            }
            g_macro.resize(g_segs.back().committedCount);
            g_segs.back().failures = MAX_FAILS / 2;
            g_backDepth++;
        }

        g_frame = g_segs.back().startFrame;
        g_holding = false;

        // volta pro checkpoint do bot (o jogo reiniciou do começo)
        if (g_segs.size() > 1 && !g_cps.empty()) {
            this->loadFromCheckpoint(g_cps.back());
        }

        makeTrial();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (g_mode != Mode::Learning || g_segs.empty()) return;
        auto p = m_player1;
        if (!p || p->m_isDead) return;

        int elapsed = g_frame - g_segs.back().startFrame;
        if (elapsed < SEG_LEN) return;

        // só cria checkpoint em lugar seguro: no chão (ou voando)
        bool stable = isFlying(p) || p->m_isOnGround;
        if (stable || elapsed >= SEG_LEN * 2) {
            commitSegment(this);
        }
    }

    void levelComplete() {
        if (g_mode == Mode::Learning && !g_segs.empty()) {
            saveExecutedInputs();

            g_mode = Mode::Replay;
            g_segs.clear();
            clearCheckpoints();

            this->refreshLabel();
            Notification::create("O bot aprendeu a fase!", NotificationIcon::Success)->show();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_mode = Mode::Off;
        g_segs.clear();
        clearCheckpoints();
        PlayLayer::onQuit();
    }
};
