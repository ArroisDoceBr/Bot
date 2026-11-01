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
#include <cstdlib>
#include <algorithm>

using namespace geode::prelude;

// ---------------------------------------------------------------
// Utilidades de texto
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
// Renumeracao de grupos e canais de cor
// ---------------------------------------------------------------
struct Remapper {
    std::map<int, int> groups;
    std::map<int, int> chans;
    int nextGroup = 4000;
    int nextChan = 500;

    int group(int g) {
        if (g <= 0) return g;
        auto it = groups.find(g);
        if (it != groups.end()) return it->second;
        if (nextGroup > 9999) return g;
        int n = nextGroup++;
        groups[g] = n;
        return n;
    }

    int chan(int c) {
        if (c < 1 || c > 999) return c; // canais especiais (1000+) ficam iguais
        auto it = chans.find(c);
        if (it != chans.end()) return it->second;
        if (nextChan > 999) return c;
        int n = nextChan++;
        chans[c] = n;
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
// Classificacao de objetos
// 0 = descartar, 1 = decoracao, 2 = hazard (vira visual), 3 = trigger de efeito
// ---------------------------------------------------------------
static int classifyObject(int id) {
    // Triggers de efeito (lista pequena, so os que tenho certeza)
    static const std::set<int> effectTriggers = {
        899,  // cor
        901,  // move
        1006, // pulse
        1007, // alpha
        1049, // toggle
        1268, // spawn
        1346, // rotate
        1347, // follow
        1520, // shake
        1585, // animate
        2067, // scale
        1818, // bg effect on
        1819  // bg effect off
    };
    if (effectTriggers.count(id)) return 3;
    if (id == 31) return 0; // start pos

    static std::unordered_map<int, int> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;

    int kind = 0;
    auto* obj = GameObject::createWithKey(id);
    if (obj) {
        if (obj->m_objectType == GameObjectType::Decoration) kind = 1;
        else if (obj->m_objectType == GameObjectType::Hazard) kind = 2;
    }
    cache[id] = kind;
    return kind;
}

// ---------------------------------------------------------------
// Converte 1 objeto da string da fase
// ---------------------------------------------------------------
static bool convertObject(
    std::string const& obj, Remapper& rm, bool spikes, bool trig, std::string& out
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

    int kind = classifyObject(id);
    if (kind == 0) return false;
    if (kind == 3 && !trig) return false;
    if (kind == 2 && !spikes) return false;

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
                if (id == 1006 && pulseMode == 0) v = std::to_string(rm.chan(toInt(v)));
                else v = std::to_string(rm.group(toInt(v)));
                break;
            case 71:
            case 395:
                v = std::to_string(rm.group(toInt(v)));
                break;
            case 21:
            case 22:
                v = std::to_string(rm.chan(toInt(v)));
                break;
            case 23:
            case 50:
                if (id == 899) v = std::to_string(rm.chan(toInt(v)));
                break;
            default:
                break;
        }
        if (!out.empty()) out += ',';
        out += std::to_string(p.first);
        out += ',';
        out += v;
    }
    if (kind == 2 && !hasNoTouch) out += ",121,1"; // sem hitbox
    return true;
}

// ---------------------------------------------------------------
// Cores do tema -> triggers de cor
// ---------------------------------------------------------------
static std::string getOr(std::map<int, std::string> const& m, int k, std::string const& def) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return def;
    return it->second;
}

static std::vector<std::string> buildColorTriggers(std::string const& header, Remapper& rm) {
    std::vector<std::string> out;
    auto parts = splitStr(header, ',');
    std::string kS38;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] == "kS38") kS38 = parts[i + 1];
    }
    if (kS38.empty()) return out;

    int row = 0;
    for (auto& entry : splitStr(kS38, '|')) {
        if (entry.empty()) continue;
        auto f = splitStr(entry, '_');
        std::map<int, std::string> m;
        for (size_t i = 0; i + 1 < f.size(); i += 2) m[toInt(f[i], -1)] = f[i + 1];

        int ch = toInt(getOr(m, 6, "0"));
        if (ch <= 0) continue;
        if (ch == 1005 || ch == 1006 || ch == 1010 || ch == 1011) continue;

        int newCh = rm.chan(ch);
        std::string s = "1,899,2,0,3," + std::to_string(15 + row * 30);
        s += ",10,0";
        s += ",23," + std::to_string(newCh);

        int copyId = toInt(getOr(m, 9, "0"));
        if (copyId > 0) {
            s += ",50," + std::to_string(rm.chan(copyId));
            s += ",49," + getOr(m, 10, "0a1a1a0a0");
            if (toInt(getOr(m, 17, "0")) == 1) s += ",60,1";
        } else {
            s += ",7," + getOr(m, 1, "255");
            s += ",8," + getOr(m, 2, "255");
            s += ",9," + getOr(m, 3, "255");
        }
        int pc = toInt(getOr(m, 4, "-1"));
        if (pc == 1) s += ",15,1";
        else if (pc == 2) s += ",16,1";

        s += ",17," + getOr(m, 5, "0");
        s += ",35," + getOr(m, 7, "1");

        out.push_back(s);
        row++;
    }
    return out;
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
                "Pronto! " + std::to_string(m_totalObjects) + " objetos colados. Salve a fase.",
                NotificationIcon::Success, 4.f
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
            bool spikes = mod->getSettingValue<bool>("spikes-visual");
            bool trig = mod->getSettingValue<bool>("copy-triggers");
            bool colors = mod->getSettingValue<bool>("copy-colors");
            size_t maxObjs = static_cast<size_t>(mod->getSettingValue<int64_t>("max-objects"));
            size_t chunkSize = static_cast<size_t>(
                std::max<int64_t>(20, mod->getSettingValue<int64_t>("chunk-size"))
            );

            Remapper rm;
            rm.nextGroup = static_cast<int>(mod->getSettingValue<int64_t>("group-base"));
            rm.nextChan = static_cast<int>(mod->getSettingValue<int64_t>("channel-base"));

            auto segs = splitStr(decoded, ';');
            std::string header = segs.empty() ? "" : segs[0];

            std::vector<std::string> good;
            for (size_t i = 1; i < segs.size(); ++i) {
                if (segs[i].empty()) continue;
                std::string o;
                if (convertObject(segs[i], rm, spikes, trig, o)) {
                    good.push_back(std::move(o));
                    if (good.size() >= maxObjs) break;
                }
            }

            if (colors) {
                for (auto& t : buildColorTriggers(header, rm)) good.push_back(t);
            }

            if (good.empty()) {
                showError("Nada para copiar dessa fase");
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
