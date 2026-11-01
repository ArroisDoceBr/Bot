#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
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

// gira o vetor (x,y) em graus no sentido horario (como o GD)
static void rotCW(float x, float y, float deg, float& ox, float& oy) {
    float r = deg * 3.14159265f / 180.f;
    float c = std::cos(r);
    float s = std::sin(r);
    ox = x * c + y * s;
    oy = -x * s + y * c;
}

// ---------------------------------------------------------------
// Listas de IDs
// ---------------------------------------------------------------

// Espinhos e serras: sao as "ancoras" do estilo
static bool isAnchor(int id) {
    static const std::set<int> a = {
        8, 9, 39, 61, 103, 392,        // espinhos
        363, 364, 365,                 // espinhos de planta
        88, 89, 98, 397, 398, 399      // serras
    };
    return a.count(id) > 0;
}

// Objetos que mudam o gameplay: nunca copiar como decoracao
static bool isBlocked(int id) {
    static const std::set<int> blocked = {
        31,
        10, 11,
        12, 13, 47, 111, 660, 745, 1331, 1933,
        45, 46,
        99, 101,
        286, 287,
        200, 201, 202, 203, 1334,
        747,
        36, 84, 141, 1022, 1330, 1333,
        1704, 1751,
        35, 67, 140, 1332,
        142, 1329,
        2063,
        3600,
        1917, 1932, 1934, 1935, 2066,
        1595, 1611, 1811, 1815, 1816, 1817,
        3614, 3615, 3617, 3619, 3620, 3641
    };
    return blocked.count(id) > 0;
}

// Triggers globais que vale copiar (nao dependem de grupos)
static bool isGlobalEffect(int id, int pulseMode) {
    if (id == 899) return true;                    // cor
    if (id == 1006 && pulseMode == 0) return true; // pulse em canal de cor
    if (id == 1520) return true;                   // shake
    if (id == 1818 || id == 1819) return true;     // efeito de fundo
    return false;
}

// ---------------------------------------------------------------
// Objeto lido da fase original
// ---------------------------------------------------------------
struct SObj {
    int id = 0;
    float x = 0.f, y = 0.f, rot = 0.f;
    bool flipY = false;
    bool trig = false;
    int pulseMode = 0;
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
            case 5: o.flipY = (toInt(v) == 1); break;
            case 6: o.rot = toFloat(v); break;
            case 36: o.trig = (toInt(v) == 1); break;
            case 52: o.pulseMode = toInt(v); break;
            default: break;
        }
    }
    if (o.id <= 0) return false;
    o.raw = s;
    return true;
}

static float effRot(SObj const& o) {
    return o.rot + (o.flipY ? 180.f : 0.f);
}

// Remove posicao, rotacao, grupos e hitbox; sobra o visual (cores, camada, etc.)
static std::string stripBase(std::string const& raw, bool dropFlips) {
    auto tok = splitStr(raw, ',');
    std::string out;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
        int k = toInt(tok[i], -1);
        switch (k) {
            case 2: case 3: case 6:
            case 57: case 274: case 442:
            case 51: case 71: case 395:
            case 33: case 108: case 121:
                continue;
            case 4: case 5:
                if (dropFlips) continue;
                break;
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
// Modelos de estilo
// ---------------------------------------------------------------
struct Piece {
    std::string base;
    float lx = 0.f, ly = 0.f, rr = 0.f;
};

struct Templ {
    std::vector<Piece> pieces;
    int count = 0;
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
// Download + processamento
// ---------------------------------------------------------------
static TaskHolder<web::WebResponse> s_holder;

static void showError(std::string const& msg) {
    Notification::create(msg, NotificationIcon::Error, 5.f)->show();
}

static void processLevel(Ref<LevelEditorLayer> editor, std::string const& decoded) {
    auto mod = Mod::get();
    bool trig = mod->getSettingValue<bool>("copy-triggers");
    bool colors = mod->getSettingValue<bool>("copy-colors");
    bool bg = mod->getSettingValue<bool>("copy-bg");
    size_t maxObjs = static_cast<size_t>(mod->getSettingValue<int64_t>("max-objects"));
    size_t chunkSize = static_cast<size_t>(
        std::max<int64_t>(20, mod->getSettingValue<int64_t>("chunk-size"))
    );
    float radius = static_cast<float>(mod->getSettingValue<int64_t>("radius"));
    size_t maxPer = static_cast<size_t>(mod->getSettingValue<int64_t>("max-per-spike"));
    size_t nTempl = static_cast<size_t>(std::max<int64_t>(1, mod->getSettingValue<int64_t>("templates")));

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

    // 2) separa ancoras e decoracao (grade espacial)
    float cell = std::max(radius, 1.f);
    auto cellKey = [&](int cx, int cy) -> long long {
        return static_cast<long long>(cx) * 1000003LL + static_cast<long long>(cy);
    };
    std::unordered_map<long long, std::vector<int>> grid;
    std::map<int, std::vector<int>> anchorsById;

    for (int i = 0; i < static_cast<int>(objs.size()); ++i) {
        auto const& o = objs[i];
        if (isAnchor(o.id)) {
            anchorsById[o.id].push_back(i);
            continue;
        }
        if (o.trig || isBlocked(o.id)) continue;
        int cx = static_cast<int>(std::floor(o.x / cell));
        int cy = static_cast<int>(std::floor(o.y / cell));
        grid[cellKey(cx, cy)].push_back(i);
    }

    if (anchorsById.empty()) {
        showError("A fase original nao tem espinhos/serras conhecidos");
        return;
    }

    // 3) monta modelos
    auto buildTempl = [&](int ai, size_t cap) -> Templ {
        Templ t;
        auto const& a = objs[ai];
        float ar = effRot(a);

        std::vector<std::pair<float, int>> near;
        int cx = static_cast<int>(std::floor(a.x / cell));
        int cy = static_cast<int>(std::floor(a.y / cell));
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                auto it = grid.find(cellKey(cx + dx, cy + dy));
                if (it == grid.end()) continue;
                for (int j : it->second) {
                    float ddx = objs[j].x - a.x;
                    float ddy = objs[j].y - a.y;
                    float d2 = ddx * ddx + ddy * ddy;
                    if (d2 <= radius * radius) near.push_back({d2, j});
                }
            }
        }
        t.count = static_cast<int>(near.size());
        std::sort(near.begin(), near.end());
        if (near.size() > cap) near.resize(cap);

        // o proprio espinho da fase original (visual, por cima do seu)
        Piece self;
        self.base = stripBase(a.raw, true);
        t.pieces.push_back(std::move(self));

        for (auto& pr : near) {
            auto const& d = objs[pr.second];
            Piece p;
            p.base = stripBase(d.raw, false);
            float lx, ly;
            rotCW(d.x - a.x, d.y - a.y, -ar, lx, ly);
            p.lx = lx;
            p.ly = ly;
            p.rr = d.rot - ar;
            t.pieces.push_back(std::move(p));
        }
        return t;
    };

    std::map<int, std::vector<Templ>> templs;
    std::vector<Templ> anyTempls;

    for (auto& kv : anchorsById) {
        auto& list = kv.second;
        size_t step = std::max<size_t>(1, list.size() / 300);
        std::vector<Templ> valid, loose;
        for (size_t i = 0; i < list.size(); i += step) {
            Templ t = buildTempl(list[i], maxPer);
            if (t.count >= 3 && t.count <= static_cast<int>(maxPer)) valid.push_back(std::move(t));
            else if (t.count >= 1) loose.push_back(std::move(t));
        }
        auto& src = valid.empty() ? loose : valid;
        std::vector<Templ> chosen;
        if (!src.empty()) {
            size_t take = std::min(nTempl, src.size());
            for (size_t k = 0; k < take; ++k) {
                size_t idx = (take == 1) ? 0 : (k * (src.size() - 1)) / (take - 1);
                chosen.push_back(src[idx]);
            }
        }
        if (!chosen.empty()) {
            if (anyTempls.empty()) anyTempls = chosen;
            templs[kv.first] = std::move(chosen);
        }
    }

    if (anyTempls.empty()) {
        showError("Nao achei decoracao perto dos espinhos da fase original");
        return;
    }

    // 4) aplica nos espinhos da sua fase
    std::vector<std::string> good;
    float userMaxX = 0.f;
    int userAnchors = 0;
    std::map<int, size_t> counter;

    CCArrayExt<GameObject*> mine(editor->m_objects);
    for (auto* o : mine) {
        if (!o) continue;
        userMaxX = std::max(userMaxX, o->getPositionX());
    }
    for (auto* o : mine) {
        if (!o) continue;
        int id = o->m_objectID;
        if (!isAnchor(id)) continue;
        if (good.size() >= maxObjs) break;

        std::vector<Templ>* list = &anyTempls;
        auto it = templs.find(id);
        if (it != templs.end()) list = &it->second;

        Templ& t = (*list)[counter[id]++ % list->size()];
        float ux = o->getPositionX();
        float uy = o->getPositionY();
        float ur = o->getRotation();
        userAnchors++;

        for (auto& p : t.pieces) {
            float ox, oy;
            rotCW(p.lx, p.ly, ur, ox, oy);
            std::string s = p.base;
            s += ",2," + numStr(ux + ox);
            s += ",3," + numStr(uy + oy);
            s += ",6," + numStr(ur + p.rr);
            s += ",121,1";
            good.push_back(std::move(s));
        }
    }

    if (userAnchors == 0) {
        showError("Nao achei espinhos/serras na sua fase");
        return;
    }

    // 5) efeitos globais, so ate perto do fim da sua fase
    if (trig) {
        for (auto const& o : objs) {
            if (!o.trig) continue;
            if (!isGlobalEffect(o.id, o.pulseMode)) continue;
            if (o.x > userMaxX + 300.f) continue;
            good.push_back(o.raw);
        }
    }

    applyHeader(editor, header, colors, bg);

    // 6) cola em partes
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
