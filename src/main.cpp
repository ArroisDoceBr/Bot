#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <cstdlib>
#include <algorithm>

using namespace geode::prelude;

// ---------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------
static std::vector<std::string> splitStr(std::string const& s, char d) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(d, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

static int toInt(std::string const& s, int def = 0) {
    if (s.empty()) return def;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    return static_cast<int>(v);
}

// ---------------------------------------------------------------
// Renumeracao de grupos
// ---------------------------------------------------------------
struct Remapper {
    std::map<int, int> groups;
    int nextGroup = 4000;

    int group(int g) {
        if (g <= 0) return g;
        auto it = groups.find(g);
        if (it != groups.end()) return it->second;
        if (nextGroup > 9999) return g;
        int n = nextGroup++;
        groups[g] = n;
        return n;
    }
};

static std::string mapGroupList(std::string const& v, Remapper& rm) {
    if (v.empty()) return v;
    auto p = splitStr(v, '.');
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (i) out += '.';
        out += std::to_string(rm.group(toInt(p[i])));
    }
    return out;
}

// ---------------------------------------------------------------
// Listas de IDs
// ---------------------------------------------------------------

// Objetos que mudam o gameplay: NAO copiar
static bool isBlocked(int id) {
    static const std::set<int> blocked = {
        31,                                   // start pos
        10, 11,                               // portais de gravidade
        12, 13, 47, 111, 660, 745, 1331, 1933,// portais de modo
        45, 46,                               // espelho
        99, 101,                              // tamanho
        286, 287,                             // dual
        200, 201, 202, 203, 1334,             // velocidade
        747,                                  // teleporte
        36, 84, 141, 1022, 1330, 1333,        // orbs
        1704, 1751,                           // orbs dash
        35, 67, 140, 1332,                    // pads
        142, 1329,                            // moedas
        2063,                                 // checkpoint
        3600,                                 // end trigger
        1917, 1932, 1934, 1935, 2066,         // reverse, player control, song, time warp, gravity
        1595, 1611, 1811, 1815, 1816, 1817,   // touch, count, colisao, pickup
        3614, 3615, 3617, 3619, 3620, 3641    // tempo e item edit
    };
    return blocked.count(id) > 0;
}

// Triggers de efeito conhecidos (nao recebem "sem hitbox")
static bool isEffectTrigger(int id) {
    static const std::set<int> trig = {
        899, 901, 1006, 1007, 1049, 1268, 1346, 1347, 1520,
        1585, 2067, 1818, 1819, 1616, 3016
    };
    return trig.count(id) > 0;
}

// ---------------------------------------------------------------
// Converte 1 objeto da string da fase
// ---------------------------------------------------------------
static bool convertObject(
    std::string const& obj, Remapper& rm, bool trig, std::string& out
) {
    auto tok = splitStr(obj, ',');
    if (tok.size() < 4) return false;

    std::vector<std::pair<int, std::string>> kv;
    int id = 0;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
        int k = toInt(tok[i], -1);
        kv.push_back({k, tok[i + 1]});
        if (k == 1) id = toInt(tok[i + 1]);
    }
    if (id <= 0) return false;
    if (isBlocked(id)) return false;

    bool isTrig = isEffectTrigger(id);
    if (isTrig && !trig) return false;

    int pulseMode = 0;
    bool hasNoTouch = false;
    for (auto& p : kv) {
        if (p.first == 52) pulseMode = toInt(p.second);
        if (p.first == 121) hasNoTouch = true;
    }

    out.clear();
    for (auto& p : kv) {
        std::string v = p.second;
        switch (p.first) {
            case 57:
            case 274:
            case 442:
                v = mapGroupList(v, rm);
                break;
            case 51:
                // pulse em canal de cor (modo 0) nao muda; o resto e grupo
                if (id == 1006 && pulseMode == 0) break;
                v = std::to_string(rm.group(toInt(v)));
                break;
            case 71:
            case 395:
                v = std::to_string(rm.group(toInt(v)));
                break;
            default:
                break;
        }
        if (!out.empty()) out += ',';
        out += std::to_string(p.first);
        out += ',';
        out += v;
    }
    // sem hitbox em tudo que nao e trigger de efeito
    if (!isTrig && !hasNoTouch) out += ",121,1";
    return true;
}

// ---------------------------------------------------------------
// Aplica cores e fundo/chao na configuracao da fase
// ---------------------------------------------------------------
static void applyHeader(
    LevelEditorLayer* editor, std::string const& header, bool colors, bool bg
) {
    auto parts = splitStr(header, ',');
    std::string kS38, kA6, kA7;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] == "kS38") kS38 = parts[i + 1];
        else if (parts[i] == "kA6") kA6 = parts[i + 1];
        else if (parts[i] == "kA7") kA7 = parts[i + 1];
    }
    auto* settings = editor->m_levelSettings;
    if (!settings) return;

    if (colors && !kS38.empty() && settings->m_effectManager) {
        settings->m_effectManager->setupFromString(kS38);
    }
    if (bg) {
        if (!kA6.empty()) settings->m_backgroundIndex = toInt(kA6, 1);
        if (!kA7.empty()) settings->m_groundIndex = toInt(kA7, 1);
    }
}

// ---------------------------------------------------------------
// No que cola as partes aos poucos (1 parte por frame)
// ---------------------------------------------------------------
class DecorPaster : public CCNode {
public:
    LevelEditorLayer* m_editor = nullptr;
    std::vector<std::string> m_chunks;
    size_t m_index = 0;
    int m_totalObjects = 0;
    Notification* m_notif = nullptr;

    static DecorPaster* create(
        LevelEditorLayer* editor, std::vector<std::string> chunks, int total, Notification* notif
    ) {
        auto r = new DecorPaster();
        if (r && r->init()) {
            r->m_editor = editor;
            r->m_chunks = std::move(chunks);
            r->m_totalObjects = total;
            r->m_notif = notif;
            r->autorelease();
            return r;
        }
        delete r;
        return nullptr;
    }

    void update(float) override {
        if (m_index >= m_chunks.size()) {
            this->unscheduleUpdate();
            if (m_notif) m_notif->hide();
            Notification::create(
                "Pronto! " + std::to_string(m_totalObjects) +
                " objetos. Salve, saia e abra a fase de novo.",
                NotificationIcon::Success, 5.f
            )->show();
            this->removeFromParentAndCleanup(true);
            return;
        }
        m_editor->createObjectsFromString(m_chunks[m_index], true, true);
        m_index++;
        if (m_notif) {
            m_notif->setString(
                "Colando " + std::to_string(m_index) + "/" + std::to_string(m_chunks.size())
            );
        }
    }
};

// ---------------------------------------------------------------
// Download + processamento
// ---------------------------------------------------------------
static TaskHolder<web::WebResponse> s_holder;

static void showError(std::string const& msg) {
    Notification::create(msg, NotificationIcon::Error, 5.f)->show();
}

static void startImport(Ref<LevelEditorLayer> editor, int levelID) {
    if (levelID <= 0) {
        showError("ID invalido");
        return;
    }
    Notification::create("Baixando fase " + std::to_string(levelID) + "...",
        NotificationIcon::Loading, 2.f)->show();

    web::WebRequest req;
    req.userAgent("");
    req.header("Content-Type", "application/x-www-form-urlencoded");
    req.bodyString("levelID=" + std::to_string(levelID) + "&secret=Wmfd2893gb7");

    s_holder.spawn(
        req.post("https://www.boomlings.com/database/downloadGJLevel22.php"),
        [editor](web::WebResponse res) {
            if (!editor->getParent()) return;
            if (!res.ok()) {
                showError("Falha no download (codigo " + std::to_string(res.code()) + ")");
                return;
            }
            std::string text = res.string().unwrapOr("");

            auto hashPos = text.find('#');
            if (hashPos != std::string::npos) text = text.substr(0, hashPos);

            auto parts = splitStr(text, ':');
            std::string raw;
            for (size_t i = 0; i + 1 < parts.size(); i += 2) {
                if (parts[i] == "4") raw = parts[i + 1];
            }
            if (raw.empty()) {
                showError("Fase nao encontrada: " + text.substr(0, 40));
                return;
            }

            std::string decoded;
            if (raw.rfind("kS", 0) == 0) {
                decoded = raw;
            } else {
                gd::string d = ZipUtils::decompressString(gd::string(raw), false, 11);
                decoded = d.c_str();
            }
            if (decoded.empty()) {
                showError("Nao consegui ler os dados da fase");
                return;
            }

            auto mod = Mod::get();
            bool trig = mod->getSettingValue<bool>("copy-triggers");
            bool colors = mod->getSettingValue<bool>("copy-colors");
            bool bg = mod->getSettingValue<bool>("copy-bg");
            size_t maxObjs = static_cast<size_t>(mod->getSettingValue<int64_t>("max-objects"));
            size_t chunkSize = static_cast<size_t>(
                std::max<int64_t>(20, mod->getSettingValue<int64_t>("chunk-size"))
            );

            Remapper rm;
            rm.nextGroup = static_cast<int>(mod->getSettingValue<int64_t>("group-base"));

            auto segs = splitStr(decoded, ';');
            std::string header = segs.empty() ? "" : segs[0];

            std::vector<std::string> good;
            for (size_t i = 1; i < segs.size(); ++i) {
                if (segs[i].empty()) continue;
                std::string o;
                if (convertObject(segs[i], rm, trig, o)) {
                    good.push_back(std::move(o));
                    if (good.size() >= maxObjs) break;
                }
            }

            applyHeader(editor, header, colors, bg);

            if (good.empty()) {
                showError("Nenhum objeto copiavel (cores aplicadas)");
                return;
            }

            std::vector<std::string> chunks;
            std::string cur;
            size_t n = 0;
            for (auto& o : good) {
                cur += o;
                cur += ';';
                if (++n >= chunkSize) {
                    chunks.push_back(cur);
                    cur.clear();
                    n = 0;
                }
            }
            if (!cur.empty()) chunks.push_back(cur);

            auto notif = Notification::create("Colando 0/" + std::to_string(chunks.size()),
                NotificationIcon::Loading, 0.f);
            notif->show();

            auto paster = DecorPaster::create(editor, std::move(chunks),
                static_cast<int>(good.size()), notif);
            if (!paster) {
                notif->hide();
                showError("Erro interno");
                return;
            }
            editor->addChild(paster);
            paster->scheduleUpdate();
        }
    );
}

// ---------------------------------------------------------------
// Botao no menu de pausa do editor
// ---------------------------------------------------------------
class $modify(DecorPauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorPauseLayer::init(editor)) return false;

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        auto spr = ButtonSprite::create("Copiar decor");
        spr->setScale(0.7f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(DecorPauseLayer::onCopyDecor)
        );
        menu->addChild(btn);
        btn->setPosition({0.f, 0.f});
        menu->setPosition({win.width - 65.f, win.height - 25.f});
        this->addChild(menu, 100);
        return true;
    }

    void onCopyDecor(CCObject*) {
        auto holder = std::make_shared<TextInput*>(nullptr);

        auto alert = createQuickPopup(
            "Copiar decoracao",
            "Digite o ID da fase:\n\n\n",
            "Cancelar", "Copiar",
            [editor = Ref<LevelEditorLayer>(m_editorLayer), holder](FLAlertLayer*, bool btn2) {
                if (!btn2) return;
                int id = 0;
                if (*holder) id = toInt((*holder)->getString());
                if (id <= 0) id = static_cast<int>(Mod::get()->getSettingValue<int64_t>("level-id"));
                startImport(editor, id);
            },
            false
        );

        auto win = CCDirector::get()->getWinSize();
        auto input = TextInput::create(160.f, "ID da fase", "bigFont.fnt");
        input->setFilter("0123456789");
        input->setMaxCharCount(10);
        input->setPosition({win.width / 2.f, win.height / 2.f - 8.f});
        alert->m_mainLayer->addChild(input);
        *holder = input;
        alert->show();
    }
};
