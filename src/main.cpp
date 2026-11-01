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
#include <cmath>
#include <cstdio>
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

static float toFloat(std::string const& s, float def = 0.f) {
    if (s.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return def;
    return static_cast<float>(v);
}

static std::string numStr(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

// troca o valor de uma chave (ou adiciona se nao existir)
static std::string setKey(std::string const& raw, int key, std::string const& val) {
    auto tok = splitStr(raw, ',');
    std::string out;
    bool found = false;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
        int k = toInt(tok[i], -1);
        std::string v = tok[i + 1];
        if (k == key) {
            v = val;
            found = true;
        }
        if (!out.empty()) out += ',';
        out += tok[i];
        out += ',';
        out += v;
    }
    if (!found) {
        if (!out.empty()) out += ',';
        out += std::to_string(key);
        out += ',';
        out += val;
    }
    return out;
}

// fica so com o visual (cores etc.): tira id, posicao, rotacao, flip, grupos, hitbox, escala, camada
static std::string stripSkin(std::string const& raw) {
    auto tok = splitStr(raw, ',');
    std::string out;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
        int k = toInt(tok[i], -1);
        switch (k) {
            case 1: case 2: case 3: case 4: case 5: case 6:
            case 24: case 25: case 32: case 33: case 108: case 121:
            case 57: case 274: case 442:
            case 51: case 71: case 395:
                continue;
            default:
                break;
        }
        if (!out.empty()) out += ',';
        out += tok[i];
        out += ',';
        out += tok[i + 1];
    }
    return out;
}

// ---------------------------------------------------------------
// Listas de IDs
// ---------------------------------------------------------------
static bool isSpike(int id) {
    static const std::set<int> a = {
        8, 9, 39, 61, 103, 392,
        363, 364, 365,
        88, 89, 98, 397, 398, 399
    };
    return a.count(id) > 0;
}

static bool isModePortal(int id) {
    static const std::set<int> p = { 12, 13, 47, 111, 660, 745, 1331, 1933 };
    return p.count(id) > 0;
}

// efeitos que dao pra copiar sem depender de grupos
static bool isCopyableEffect(int id, int pulseMode, bool shake) {
    if (id == 899) return true;                    // cor
    if (id == 1006 && pulseMode == 0) return true; // pulse em canal de cor
    if (id == 1818 || id == 1819) return true;     // efeito de fundo
    if (id == 1520 && shake) return true;          // shake
    return false;
}

static bool isTriggerLike(int id) {
    return id == 899 || id == 901 || id == 1006 || id == 1007 || id == 1049 ||
           id == 1268 || id == 1346 || id == 1347 || id == 1520 || id == 1585 ||
           id == 1616 || id == 1818 || id == 1819 || id == 2067 || id == 3016 ||
           id == 3600 || id == 31;
}

// ---------------------------------------------------------------
// Leitura de objeto da fase original
// ---------------------------------------------------------------
struct SObj {
    int id = 0;
    float x = 0.f, y = 0.f;
    int pulseMode = 0;
    bool touch = false;
    bool spawn = false;
    std::string raw;
};

static bool parseObj(std::string const& s, SObj& o) {
    auto tok = splitStr(s, ',');
    if (tok.size() < 4) return false;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
        int k = toInt(tok[i], -1);
        auto const& v = tok[i + 1];
        switch (k) {
            case 1: o.id = toInt(v); break;
            case 2: o.x = toFloat(v); break;
            case 3: o.y = toFloat(v); break;
            case 11: o.touch = (toInt(v) == 1); break;
            case 52: o.pulseMode = toInt(v); break;
            case 62: o.spawn = (toInt(v) == 1); break;
            default: break;
        }
    }
    if (o.id <= 0) return false;
    o.raw = s;
    return true;
}

static std::string bestOf(std::map<std::string, int> const& m) {
    std::string best;
    int bc = 0;
    for (auto& kv : m) {
        if (kv.second > bc) {
            bc = kv.second;
            best = kv.first;
        }
    }
    return best;
}

// ---------------------------------------------------------------
// Mapeamento de tempo (x da original -> x da sua fase)
// ---------------------------------------------------------------
struct XMap {
    std::vector<std::pair<float, float>> pts; // (x original, x seu), crescente

    float map(float x) const {
        if (pts.empty()) return x;
        if (x <= pts.front().first) return x - pts.front().first + pts.front().second;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            float a = pts[i].first, b = pts[i + 1].first;
            if (x >= a && x < b) {
                float t = (x - a) / (b - a);
                return pts[i].second + t * (pts[i + 1].second - pts[i].second);
            }
        }
        return pts.back().second + (x - pts.back().first);
    }
};

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
// Processamento
// ---------------------------------------------------------------
static TaskHolder<web::WebResponse> s_holder;

static void showError(std::string const& msg) {
    Notification::create(msg, NotificationIcon::Error, 5.f)->show();
}

static void processLevel(Ref<LevelEditorLayer> editor, std::string const& decoded) {
    auto mod = Mod::get();
    bool colors = mod->getSettingValue<bool>("copy-colors");
    bool bg = mod->getSettingValue<bool>("copy-bg");
    bool skin = mod->getSettingValue<bool>("copy-spike-skin");
    bool effects = mod->getSettingValue<bool>("copy-effects");
    bool shake = mod->getSettingValue<bool>("copy-shake");
    bool align = mod->getSettingValue<bool>("align-portals");
    bool delMine = mod->getSettingValue<bool>("delete-my-color-triggers");
    int zorder = static_cast<int>(mod->getSettingValue<int64_t>("overlay-zorder"));
    size_t maxObjs = static_cast<size_t>(mod->getSettingValue<int64_t>("max-objects"));
    size_t chunkSize = static_cast<size_t>(
        std::max<int64_t>(20, mod->getSettingValue<int64_t>("chunk-size"))
    );

    // 1) le a fase original
    auto segs = splitStr(decoded, ';');
    std::string header = segs.empty() ? "" : segs[0];

    std::vector<SObj> objs;
    objs.reserve(segs.size());
    for (size_t i = 1; i < segs.size(); ++i) {
        if (segs[i].empty()) continue;
        SObj o;
        if (parseObj(segs[i], o)) objs.push_back(std::move(o));
    }
    if (objs.empty()) {
        showError("A fase original esta vazia");
        return;
    }

    // 2) visual dos espinhos (o mais comum da fase original)
    std::map<int, std::map<std::string, int>> byId;
    std::map<std::string, int> allSkins;
    std::vector<std::pair<float, int>> origPortals;
    float origEnd = 0.f;

    for (auto const& o : objs) {
        if (isSpike(o.id)) {
            std::string sk = stripSkin(o.raw);
            byId[o.id][sk]++;
            allSkins[sk]++;
        }
        if (isModePortal(o.id)) origPortals.push_back({o.x, o.id});
        if (!isTriggerLike(o.id)) origEnd = std::max(origEnd, o.x);
    }
    std::sort(origPortals.begin(), origPortals.end());
    std::string anySkin = bestOf(allSkins);

    // 3) le a sua fase
    CCArray* arr = editor->m_objects;
    std::vector<GameObject*> mine;
    if (arr) {
        for (int i = 0; i < arr->count(); ++i) {
            auto* o = static_cast<GameObject*>(arr->objectAtIndex(i));
            if (o) mine.push_back(o);
        }
    }
    if (mine.empty()) {
        showError("Sua fase esta vazia");
        return;
    }

    float userEnd = 0.f;
    std::vector<std::pair<float, int>> userPortals;
    std::vector<GameObject*> myColorTriggers;
    for (auto* o : mine) {
        int id = o->m_objectID;
        if (id == 899) {
            myColorTriggers.push_back(o);
            continue;
        }
        if (!isTriggerLike(id)) userEnd = std::max(userEnd, o->getPositionX());
        if (isModePortal(id)) userPortals.push_back({o->getPositionX(), id});
    }
    std::sort(userPortals.begin(), userPortals.end());
    if (userEnd <= 0.f) userEnd = 1.f;
    if (origEnd <= 0.f) origEnd = 1.f;

    // 4) mapa de tempo
    XMap xm;
    xm.pts.push_back({0.f, 0.f});
    if (align) {
        size_t j = 0;
        float lastO = 0.f, lastU = 0.f;
        for (auto& up : userPortals) {
            size_t k = j;
            while (k < origPortals.size() && origPortals[k].second != up.second) ++k;
            if (k >= origPortals.size()) continue;
            float ox = origPortals[k].first;
            float ux = up.first;
            if (ox > lastO && ux > lastU) {
                xm.pts.push_back({ox, ux});
                lastO = ox;
                lastU = ux;
            }
            j = k + 1;
        }
        if (origEnd > lastO && userEnd > lastU) xm.pts.push_back({origEnd, userEnd});
    } else {
        xm.pts.push_back({origEnd, userEnd});
    }

    // 5) monta o que sera colado
    std::vector<std::string> good;

    if (skin && !anySkin.empty()) {
        for (auto* o : mine) {
            int id = o->m_objectID;
            if (!isSpike(id)) continue;
            if (good.size() >= maxObjs) break;
            std::string sk = anySkin;
            auto it = byId.find(id);
            if (it != byId.end()) sk = bestOf(it->second);
            std::string s = setKey(sk, 1, std::to_string(id));
            s += ",2," + numStr(o->getPositionX());
            s += ",3," + numStr(o->getPositionY());
            s += ",6," + numStr(o->getRotation());
            s += ",25," + std::to_string(zorder);
            s += ",121,1";
            good.push_back(std::move(s));
        }
    }

    int effCount = 0;
    if (effects) {
        for (auto const& o : objs) {
            if (!isCopyableEffect(o.id, o.pulseMode, shake)) continue;
            if (o.touch || o.spawn) continue;
            if (good.size() >= maxObjs) break;
            float nx = xm.map(o.x);
            if (nx > userEnd + 300.f) continue;
            good.push_back(setKey(o.raw, 2, numStr(nx)));
            effCount++;
        }
    }

    // 6) apaga os seus triggers de cor (so se tem efeitos novos pra colocar no lugar)
    if (effects && delMine && effCount > 0) {
        for (auto* o : myColorTriggers) {
            editor->removeObject(o, true);
        }
    }

    applyHeader(editor, header, colors, bg);

    if (good.empty()) {
        Notification::create("Cores e fundo aplicados. Nada pra colar.",
            NotificationIcon::Success, 4.f)->show();
        return;
    }

    // 7) cola em partes
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

            processLevel(editor, decoded);
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
            "Copiar tema",
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
