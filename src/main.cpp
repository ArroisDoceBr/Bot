#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/ui/Notification.hpp>

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

struct WavePoint {
    float x;
    float y;
};

struct SpikeJob {
    float x;
    float y;
    float rot;
    bool flip;
};

static std::vector<WavePoint> s_path;

// ---------------------------------------------------------------
// Monta a lista de espinhos a partir do caminho gravado
// ---------------------------------------------------------------
static std::vector<SpikeJob> buildJobs(float gap, float step) {
    std::vector<SpikeJob> jobs;
    if (s_path.size() < 2) return jobs;

    float lastX = -1e9f;
    for (size_t i = 1; i < s_path.size(); ++i) {
        auto const& p = s_path[i];
        auto const& q = s_path[i - 1];
        if (p.x - lastX < step) continue;

        float dy = p.y - q.y;
        float rot = 0.f;
        if (dy > 0.8f) rot = -45.f;       // subindo
        else if (dy < -0.8f) rot = 45.f;  // descendo

        jobs.push_back({p.x, p.y - gap / 2.f, rot, false}); // chao
        jobs.push_back({p.x, p.y + gap / 2.f, rot, true});  // teto
        lastX = p.x;
    }
    return jobs;
}

// ---------------------------------------------------------------
// No que coloca os espinhos aos poucos (1 lote por frame)
// ---------------------------------------------------------------
class WaveBuilder : public CCNode {
public:
    Ref<LevelEditorLayer> m_editor;
    std::vector<SpikeJob> m_jobs;
    size_t m_index = 0;
    size_t m_perFrame = 60;
    Notification* m_notif = nullptr;

    static WaveBuilder* create(
        LevelEditorLayer* editor, std::vector<SpikeJob> jobs,
        size_t perFrame, Notification* notif
    ) {
        auto r = new WaveBuilder();
        if (r && r->init()) {
            r->m_editor = editor;
            r->m_jobs = std::move(jobs);
            r->m_perFrame = perFrame;
            r->m_notif = notif;
            r->autorelease();
            return r;
        }
        delete r;
        return nullptr;
    }

    void update(float) override {
        if (!m_editor || !m_editor->getParent()) {
            this->unscheduleUpdate();
            if (m_notif) m_notif->hide();
            this->removeFromParentAndCleanup(true);
            return;
        }

        size_t end = std::min(m_index + m_perFrame, m_jobs.size());
        for (; m_index < end; ++m_index) {
            auto const& j = m_jobs[m_index];
            auto* obj = m_editor->createObject(8, CCPoint{j.x, j.y}, true);
            if (obj) {
                obj->setRotation(j.rot);
                obj->setFlipY(j.flip);
            }
        }

        if (m_notif) {
            m_notif->setString(
                "Colando " + std::to_string(m_index) + "/" + std::to_string(m_jobs.size())
            );
        }

        if (m_index >= m_jobs.size()) {
            this->unscheduleUpdate();
            if (m_notif) m_notif->hide();
            Notification::create(
                "Pronto! " + std::to_string(m_jobs.size()) + " espinhos. Salve a fase.",
                NotificationIcon::Success, 5.f
            )->show();
            this->removeFromParentAndCleanup(true);
        }
    }
};

// ---------------------------------------------------------------
// Grava a posicao da wave durante o playtest do editor
// ---------------------------------------------------------------
class $modify(WavePlayer, PlayerObject) {
    void updateJump(float dt) {
        PlayerObject::updateJump(dt);

        if (!m_isDart) return;
        auto editor = LevelEditorLayer::get();
        if (!editor || PlayLayer::get()) return;
        if (this != editor->m_player1) return;
        if (!Mod::get()->getSettingValue<bool>("record-wave")) return;

        auto pos = this->getPosition();
        if (!s_path.empty()) {
            float lastX = s_path.back().x;
            if (pos.x < lastX - 50.f) {
                s_path.clear();            // recomecou o playtest
            } else if (pos.x - lastX < 2.f) {
                return;                    // quase nao andou
            }
        }
        s_path.push_back({pos.x, pos.y});
    }
};

// ---------------------------------------------------------------
// Ao parar o playtest, gera o corredor
// ---------------------------------------------------------------
class $modify(WaveEditor, LevelEditorLayer) {
    void onStopPlaytest() {
        LevelEditorLayer::onStopPlaytest();

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("record-wave") || s_path.size() < 2) {
            s_path.clear();
            return;
        }

        float gap = static_cast<float>(mod->getSettingValue<double>("gap"));
        float step = static_cast<float>(mod->getSettingValue<double>("step"));
        size_t perFrame = static_cast<size_t>(
            std::max<int64_t>(10, mod->getSettingValue<int64_t>("per-frame"))
        );

        auto jobs = buildJobs(gap, step);
        s_path.clear();
        if (jobs.empty()) return;

        auto notif = Notification::create(
            "Gerando " + std::to_string(jobs.size()) + " espinhos...",
            NotificationIcon::Loading, 0.f
        );
        notif->show();

        auto builder = WaveBuilder::create(this, std::move(jobs), perFrame, notif);
        if (!builder) {
            notif->hide();
            return;
        }
        this->addChild(builder);
        builder->scheduleUpdate();
    }
};
