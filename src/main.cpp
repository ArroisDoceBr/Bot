// Auto Learn Bot - robô que aprende a fase por tentativa e erro.
// Funciona no modo prática (não vale como progresso oficial).
//
// Como funciona:
//  1. A fase é dividida em trechos curtos (~0.5s).
//  2. Em cada trecho o bot testa cliques aleatórios.
//  3. Se morre, volta pro checkpoint e testa outro padrão.
//  4. Se passa o trecho, salva os cliques, cria um checkpoint e segue.
//  5. Se falha demais, volta um trecho e tenta outra saída.
//  6. Ao completar a fase, entra em modo REPLAY e repete tudo sozinho.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
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

    constexpr int SEG_LEN = 120;  // tamanho de cada trecho em ticks (~0.5s a 240 TPS)
    constexpr int MAX_FAILS = 80; // falhas antes de voltar um trecho

    Mode g_mode = Mode::Off;
    int g_frame = 0;
    bool g_holding = false;
    std::vector<Event> g_macro; // cliques confirmados (a fase aprendida)
    std::vector<Event> g_trial; // cliques da tentativa atual
    size_t g_trialIdx = 0;
    size_t g_replayIdx = 0;
    std::vector<Segment> g_segs;
    std::mt19937 g_rng{std::random_device{}()};

    // Gera um padrão aleatório de cliques para o trecho atual.
    void makeTrial() {
        g_trial.clear();
        g_trialIdx = 0;
        if (g_segs.empty()) return;

        auto const& seg = g_segs.back();
        static const float probs[] = {0.0f, 0.004f, 0.01f, 0.02f, 0.04f};
        std::uniform_int_distribution<int> pick(0, 4);
        std::uniform_real_distribution<float> chance(0.f, 1.f);

        float p = probs[pick(g_rng)];
        bool h = seg.startHolding;
        for (int t = seg.startFrame + 1; t < seg.startFrame + SEG_LEN; ++t) {
            if (chance(g_rng) < p) {
                h = !h;
                g_trial.push_back({t, h});
            }
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

    // O trecho foi vencido: salva os cliques, cria checkpoint e começa o próximo.
    void commitSegment(PlayLayer* pl) {
        auto const seg = g_segs.back();
        if (seg.startHolding) g_macro.push_back({seg.startFrame, true});
        for (auto const& e : g_trial) g_macro.push_back(e);

        Segment next;
        next.startFrame = g_frame;
        next.startHolding = g_holding;
        next.committedCount = g_macro.size();
        g_segs.push_back(next);

        auto cp = pl->createCheckpoint();
        pl->storeCheckpoint(cp);

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
            while (m_checkpointArray->count() > 0) m_checkpointArray->removeLastObject();

            g_segs.clear();
            g_macro.clear();
            g_frame = 0;
            g_holding = false;
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
            g_segs.push_back(Segment{});
        } else if (g_frame > g_segs.back().startFrame) {
            g_segs.back().failures++;
        }

        // falhou demais neste trecho: volta um trecho e tenta outra saída
        if (g_segs.back().failures >= MAX_FAILS && g_segs.size() > 1) {
            g_segs.pop_back();
            g_macro.resize(g_segs.back().committedCount);
            g_segs.back().failures = MAX_FAILS / 2;
            g_frame = g_segs.back().startFrame;
            m_checkpointArray->removeLastObject();
            PlayLayer::resetLevel();
        }

        g_frame = g_segs.back().startFrame;
        g_holding = false;
        makeTrial();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (g_mode != Mode::Learning || g_segs.empty()) return;
        if (m_player1->m_isDead) return;

        if (g_frame >= g_segs.back().startFrame + SEG_LEN) {
            commitSegment(this);
        }
    }

    void levelComplete() {
        if (g_mode == Mode::Learning && !g_segs.empty()) {
            auto const seg = g_segs.back();
            if (seg.startHolding) g_macro.push_back({seg.startFrame, true});
            for (auto const& e : g_trial) {
                if (e.frame <= g_frame) g_macro.push_back(e);
            }

            g_mode = Mode::Replay;
            g_segs.clear();
            while (m_checkpointArray->count() > 0) m_checkpointArray->removeLastObject();

            this->refreshLabel();
            Notification::create("O bot aprendeu a fase!", NotificationIcon::Success)->show();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_mode = Mode::Off;
        g_segs.clear();
        PlayLayer::onQuit();
    }
};
