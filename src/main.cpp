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

// gira (x,y) em graus no sentido horario (como o GD)
static void rotCW(float x, float y, float deg, float& ox, float& oy) {
    float r = deg * 3.14159265f / 180.f;
    float c = std::cos(r);
    float s = std::sin(r);
    ox = x * c + y * s;
    oy = -x * s + y * c;
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

// tira posicao, rotacao, grupos e hitbox; sobra id, cores, camada
static std::string stripPiece(std::string const& raw, bool keepFlips) {
    auto tok = splitStr(raw, ',');
    std::string out;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
        int k = toInt(tok[i], -1);
        switch (k) {
            case 2: case 3: case 6:
            case 33: case 108: case 121:
            case 57: case 274: case 442:
            case 51: case 71: case 395:
                continue;
            case 4: case 5:
                if (!keepFlips) continue;
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
// Classificacao de objetos: 1 = bloco, 2 = espinho/serra, 3 = decoracao
// ---------------------------------------------------------------
static int classify(int id) {
    static std::unordered_map<int, int> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    int kind = 0;
    auto* obj = GameObject::createWithKey(id);
    if (obj) {
        auto t = obj->m_objectType;
        if (t == GameObjectType::Solid || t == GameObjectType::Slope) kind = 1;
        else if (t == GameObjectType::Hazard) kind = 2;
        else if (t == GameObjectType::Decoration) kind = 3;
    }
    cache[id] = kind;
    return kind;
}

static bool isModePortal(int id) {
    static const std::set<int> p = { 12, 13, 47, 111, 660, 745, 1331, 1933 };
    return p.count(id) > 0;
}

static bool isCopyableEffect(int id, int pulseMode, bool shake) {
    if (id == 899) return true;
    if (id == 1006 && pulseMode == 0) return true;
    if (id == 1818 || id == 1819) return true;
    if (id == 1520 && shake) return true;
    return false;
}

static bool isTriggerLike(int id) {
    return id == 899 || id == 901 || id == 1006 || id == 1007 || id == 1049 ||
           id == 1268 || id == 1346 || id == 1347 || id == 1520 || id == 1585 ||
           id == 1616 || id == 1818 || id == 1819 || id == 2067 || id == 3016 ||
           id == 3600 || id == 31;
}

// ---------------------------------------------------------------
// Objeto da fase original
// ---------------------------------------------------------------
struct SObj {
    int id = 0;
    float x = 0.f, y = 0.f, rot = 0.f;
    bool flipY = false;
    bool trig = false;
    bool touch = false;
    bool spawn = false;
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
            case 11: o.touch = (toInt(v) == 1); break;
            case 36: o.trig = (toInt(v) == 1); break;
            case 52: o.pulseMode = toInt(v); break;
            case 62: o.spawn = (toInt(v) == 1); break;
            default: break;
        }
    }
    if (o.id <= 0) return false;
    o.raw = s;
    return true;
}

// ---------------------------------------------------------------
// Padroes
// ---------------------------------------------------------------
struct Piece {
    std::string base;
    float lx = 0.f, ly = 0.f, rr = 0.f;
};

struct Templ {
    std::vector<Piece> pieces; // pieces[0] = o proprio objeto original
    int decor = 0;
};

typedef std::map<std::string, std::pair<int, Templ>> PatMap;

static std::vector<Templ> pickTop(PatMap const& m, size_t n) {
    std::vector<std::pair<int, Templ const*>> v;
    for (auto& kv : m) v.push_back({kv.second.first, &kv.second.second});
    std::sort(v.begin(), v.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    std::vector<Templ> out;
    for (size_t i = 0; i < v.size() && out.size() < n; ++i) out.push_back(*v[i].second);
    return out;
}

// ---------------------------------------------------------------
// Mapa de tempo (x da original -> x da sua fase)
// ---------------------------------------------------------------
struct XMap {
    std::vector<std::pair<float, float>> pts;

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
// Cores e fundo/chao
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
    bool themeOnly = mod->getSettingValue<bool>("theme-only");
    bool colors = mod->getSettingValue<bool>("copy-colors");
    bool bg = mod->getSettingValue<bool>("copy-bg");
    bool patSpikes = mod->getSettingValue<bool>("pattern-spikes");
    bool patBlocks = mod->getSettingValue<bool>("pattern-blocks");
    bool effects = mod->getSettingValue<bool>("copy-effects");
    bool shake = mod->getSettingValue<bool>("copy-shake");
    bool align = mod->getSettingValue<bool>("align-portals");
    int zorder = static_cast<int>(mod->getSettingValue<int64_t>("overlay-zorder"));
    float radius = static_cast<float>(mod->getSettingValue<int64_t>("radius"));
    size_t maxPer = static_cast<size_t>(mod->getSettingValue<int64_t>("max-per-object"));
    size_t nTempl = static_cast<size_t>(std::max<int64_t>(1, mod->getSettingValue<int64_t>("templates")));
    size_t maxObjs = static_cast<size_t>(mod->getSettingValue<int64_t>("max-objects"));
    size_t chunkSize = static_cast<size_t>(
        std::max<int64_t>(20, mod->getSettingValue<int64_t>("chunk-size"))
    );

    auto segs = splitStr(decoded, ';');
    std::string header = segs.empty() ? "" : segs[0];

    // ---------- modo so tema ----------
    if (themeOnly) {
        applyHeader(editor, header, colors, bg);
        Notification::create(
            "Tema aplicado! Salve, saia e abra a fase de novo.",
            NotificationIcon::Success, 5.f
        )->show();
        return;
    }

    // ---------- 1) le a fase original ----------
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

    // ---------- 2) grade de decoracao + lista de ancoras ----------
    float cell = std::max(radius, 1.f);
    auto cellKey = [&](int cx, int cy) -> long long {
        return static_cast<long long>(cx) * 1000003LL + static_cast<long long>(cy);
    };
    std::unordered_map<long long, std::vector<int>> grid;
    std::map<int, std::vector<int>> spikeById;
    std::vector<int> blockList;

    std::vector<std::pair<float, int>> origPortals;
    float origEnd = 0.f;

    for (int i = 0; i < static_cast<int>(objs.size()); ++i) {
        auto const& o = objs[i];
        if (isModePortal(o.id)) origPortals.push_back({o.x, o.id});
        if (!isTriggerLike(o.id)) origEnd = std::max(origEnd, o.x);
        if (o.trig || isTriggerLike(o.id)) continue;
        int kind = classify(o.id);
        if (kind == 2) spikeById[o.id].push_back(i);
        else if (kind == 1) blockList.push_back(i);
        else if (kind == 3) {
            int cx = static_cast<int>(std::floor(o.x / cell));
            int cy = static_cast<int>(std::floor(o.y / cell));
            grid[cellKey(cx, cy)].push_back(i);
        }
    }
    std::sort(origPortals.begin(), origPortals.end());

    // ---------- 3) monta e conta os padroes ----------
    auto buildTempl = [&](int ai, std::string& sig) -> Templ {
        Templ t;
        auto const& a = objs[ai];
        float ar = a.rot + (a.flipY ? 180.f : 0.f);

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
        std::sort(near.begin(), near.end());
        if (near.size() > maxPer) near.resize(maxPer);

        Piece self;
        self.base = stripPiece(a.raw, false);
        t.pieces.push_back(self);

        std::vector<std::string> parts;
        for (auto& pr : near) {
            auto const& d = objs[pr.second];
            Piece p;
            p.base = stripPiece(d.raw, true);
            float lx, ly;
            rotCW(d.x - a.x, d.y - a.y, -ar, lx, ly);
            p.lx = lx;
            p.ly = ly;
            p.rr = d.rot - ar;
            parts.push_back(
                p.base + "|" + std::to_string(static_cast<int>(std::lround(lx / 7.5f))) +
                "|" + std::to_string(static_cast<int>(std::lround(ly / 7.5f))) +
                "|" + std::to_string(static_cast<int>(std::lround(p.rr / 15.f)))
            );
            t.pieces.push_back(std::move(p));
        }
        t.decor = static_cast<int>(near.size());
        std::sort(parts.begin(), parts.end());
        sig = self.base + "#";
        for (auto& s : parts) {
            sig += s;
            sig += "@";
        }
        return t;
    };

    auto addPattern = [&](PatMap& m, int ai) {
        std::string sig;
        Templ t = buildTempl(ai, sig);
        if (t.decor < 1) return;
        auto it = m.find(sig);
        if (it == m.end()) m[sig] = {1, std::move(t)};
        else it->second.first++;
    };

    std::map<int, std::vector<Templ>> spikeTempls;
    std::vector<Templ> spikeAny, blockAny;

    if (patSpikes) {
        PatMap all;
        for (auto& kv : spikeById) {
            PatMap pm;
            size_t step = std::max<size_t>(1, kv.second.size() / 1500);
            for (size_t i = 0; i < kv.second.size(); i += step) {
                addPattern(pm, kv.second[i]);
                addPattern(all, kv.second[i]);
            }
            auto top = pickTop(pm, nTempl);
            if (!top.empty()) spikeTempls[kv.first] = std::move(top);
        }
        spikeAny = pickTop(all, nTempl);
    }
    if (patBlocks && !blockList.empty()) {
        PatMap pm;
        size_t step = std::max<size_t>(1, blockList.size() / 3000);
        for (size_t i = 0; i < blockList.size(); i += step) addPattern(pm, blockList[i]);
        blockAny = pickTop(pm, nTempl);
    }

    // ---------- 4) le a sua fase ----------
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
    for (auto* o : mine) {
        int id = o->m_objectID;
        if (!isTriggerLike(id)) userEnd = std::max(userEnd, o->getPositionX());
        if (isModePortal(id)) userPortals.push_back({o->getPositionX(), id});
    }
    std::sort(userPortals.begin(), userPortals.end());
    if (userEnd <= 0.f) userEnd = 1.f;
    if (origEnd <= 0.f) origEnd = 1.f;

    // ---------- 5) aplica os padroes nos seus objetos ----------
    std::vector<std::string> good;
    std::map<int, size_t> counter;
    int decorated = 0;

    auto emit = [&](Templ const& t, GameObject* o) {
        float ux = o->getPositionX();
        float uy = o->getPositionY();
        float ur = o->getRotation();
        for (size_t k = 0; k < t.pieces.size(); ++k) {
            auto const& p = t.pieces[k];
            float ox, oy;
            rotCW(p.lx, p.ly, ur, ox, oy);
            std::string s = p.base;
            s += ",2," + numStr(ux + ox);
            s += ",3," + numStr(uy + oy);
            s += ",6," + numStr(ur + p.rr);
            if (k == 0) s = setKey(s, 25, std::to_string(zorder));
            s += ",121,1";
            good.push_back(std::move(s));
        }
        decorated++;
    };

    if (patSpikes || patBlocks) {
        for (auto* o : mine) {
            if (good.size() >= maxObjs) break;
            int id = o->m_objectID;
            if (isTriggerLike(id)) continue;
            auto type = o->m_objectType;

            if (patSpikes && type == GameObjectType::Hazard) {
                std::vector<Templ>* list = &spikeAny;
                auto it = spikeTempls.find(id);
                if (it != spikeTempls.end()) list = &it->second;
                if (list->empty()) continue;
                emit((*list)[counter[id]++ % list->size()], o);
            } else if (patBlocks && (type == GameObjectType::Solid || type == GameObjectType::Slope)) {
                if (blockAny.empty()) continue;
                emit(blockAny[counter[-1]++ % blockAny.size()], o);
            }
        }
    }

    // ---------- 6) efeitos no tempo certo ----------
    int effCount = 0;
    if (effects) {
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
            if (origEnd > lastO && userEnd > lastU) xm.pts.push_back({origEnd, us
