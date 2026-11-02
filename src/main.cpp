
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <memory>

using namespace geode::prelude;

// ============================================================
// DECOR COPY V2
// Parte 1 - leitura, classificacao e estruturas
// ============================================================

static std::vector<std::string> splitStr(
    std::string const& s, char delimiter
) {
    std::vector<std::string> result;
    size_t start = 0;

    while (true) {
        size_t pos = s.find(delimiter, start);

        if (pos == std::string::npos) {
            result.push_back(s.substr(start));
            break;
        }

        result.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }

    return result;
}

static int toInt(std::string const& s, int fallback = 0) {
    if (s.empty()) return fallback;

    char* end = nullptr;
    long value = std::strtol(s.c_str(), &end, 10);

    if (end == s.c_str()) return fallback;

    return static_cast<int>(value);
}

static float toFloat(std::string const& s, float fallback = 0.f) {
    if (s.empty()) return fallback;

    char* end = nullptr;
    double value = std::strtod(s.c_str(), &end);

    if (end == s.c_str()) return fallback;

    return static_cast<float>(value);
}

static std::string numStr(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

static void rotatePoint(
    float x, float y, float degrees,
    float& outX, float& outY
) {
    float radians = degrees * 3.14159265f / 180.f;

    float c = std::cos(radians);
    float s = std::sin(radians);

    outX = x * c + y * s;
    outY = -x * s + y * c;
}

static std::string setKey(
    std::string const& raw,
    int wantedKey,
    std::string const& value
) {
    auto tokens = splitStr(raw, ',');
    std::string result;
    bool found = false;

    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        int key = toInt(tokens[i], -1);
        std::string val = tokens[i + 1];

        if (key == wantedKey) {
            val = value;
            found = true;
        }

        if (!result.empty()) result += ',';

        result += tokens[i];
        result += ',';
        result += val;
    }

    if (!found) {
        if (!result.empty()) result += ',';

        result += std::to_string(wantedKey);
        result += ',';
        result += value;
    }

    return result;
}

// Remove propriedades que nao devem ser copiadas
// para outro objeto do editor.
static std::string cleanDecoration(std::string const& raw) {
    auto tokens = splitStr(raw, ',');
    std::string result;

    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        int key = toInt(tokens[i], -1);

        switch (key) {
            case 2:   // X
            case 3:   // Y
            case 6:   // Rotacao
            case 33:
            case 51:
            case 57:
            case 71:
            case 108:
            case 121:
            case 274:
            case 395:
            case 442:
                continue;

            default:
                break;
        }

        if (!result.empty()) result += ',';

        result += tokens[i];
        result += ',';
        result += tokens[i + 1];
    }

    return result;
}

// ============================================================
// OBJETOS
// ============================================================

struct LevelObject {
    int id = 0;

    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f;

    bool flipY = false;
    bool trigger = false;

    int kind = 0;

    std::string raw;
};

static bool parseObject(
    std::string const& raw,
    LevelObject& object
) {
    auto tokens = splitStr(raw, ',');

    if (tokens.size() < 4) return false;

    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        int key = toInt(tokens[i], -1);
        auto const& value = tokens[i + 1];

        switch (key) {
            case 1:
                object.id = toInt(value);
                break;

            case 2:
                object.x = toFloat(value);
                break;

            case 3:
                object.y = toFloat(value);
                break;

            case 5:
                object.flipY = toInt(value) == 1;
                break;

            case 6:
                object.rotation = toFloat(value);
                break;

            case 11:
            case 36:
                if (toInt(value) == 1) object.trigger = true;
                break;

            default:
                break;
        }
    }

    if (object.id <= 0) return false;

    object.raw = raw;
    return true;
}

// 1 = bloco
// 2 = perigo
// 3 = decoracao

static int classifyObject(int id) {
    static std::unordered_map<int, int> cache;

    auto found = cache.find(id);

    if (found != cache.end()) {
        return found->second;
    }

    int result = 0;

    auto* object = GameObject::createWithKey(id);

    if (object) {
        auto type = object->m_objectType;

        if (
            type == GameObjectType::Solid ||
            type == GameObjectType::Slope
        ) {
            result = 1;
        }
        else if (type == GameObjectType::Hazard) {
            result = 2;
        }
        else if (type == GameObjectType::Decoration) {
            result = 3;
        }
    }

    cache[id] = result;

    return result;
}

// ============================================================
// ESTRUTURAS DE DECORACAO
// ============================================================

struct DecorationPiece {
    std::string raw;

    float localX = 0.f;
    float localY = 0.f;
    float relativeRotation = 0.f;
};

struct DecorationPattern {
    int anchorID = 0;
    int anchorKind = 0;

    std::vector<DecorationPiece> pieces;
};

struct SpatialAnchor {
    int objectIndex = -1;
    int id = 0;
    int kind = 0;

    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f;
    bool flipY = false;
};

struct SpatialGrid {
    float cellSize = 90.f;

    std::unordered_map<long long, std::vector<int>> cells;

    long long key(int x, int y) const {
        return static_cast<long long>(x) * 1000003LL + y;
    }

    void insert(float x, float y, int index) {
        int cx = static_cast<int>(std::floor(x / cellSize));
        int cy = static_cast<int>(std::floor(y / cellSize));

        cells[key(cx, cy)].push_back(index);
    }

    std::vector<int> nearby(float x, float y) const {
        std::vector<int> result;

        int cx = static_cast<int>(std::floor(x / cellSize));
        int cy = static_cast<int>(std::floor(y / cellSize));

        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                auto it = cells.find(key(cx + dx, cy + dy));

                if (it == cells.end()) continue;

                result.insert(
                    result.end(),
                    it->second.begin(),
                    it->second.end()
                );
            }
        }

        return result;
    }
};

// ============================================================
// CORES E FUNDO
// ============================================================

static void applyHeader(
    LevelEditorLayer* editor,
    std::string const& header,
    bool copyColors,
    bool copyBackground
) {
    auto tokens = splitStr(header, ',');

    std::string colorData;
    std::string backgroundData;
    std::string groundData;

    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "kS38") {
            colorData = tokens[i + 1];
        }
        else if (tokens[i] == "kA6") {
            backgroundData = tokens[i + 1];
        }
        else if (tokens[i] == "kA7") {
            groundData = tokens[i + 1];
        }
    }

    auto* settings = editor->m_levelSettings;

    if (!settings) return;

    if (
        copyColors &&
        !colorData.empty() &&
        settings->m_effectManager
    ) {
        settings->m_effectManager->setupFromString(colorData);
    }

    if (copyBackground) {
        if (!backgroundData.empty()) {
            settings->m_backgroundIndex = toInt(backgroundData, 1);
        }

        if (!groundData.empty()) {
            settings->m_groundIndex = toInt(groundData, 1);
        }
    }
}

// ============================================================
// COLAGEM EM PARTES
// ============================================================

class DecorPaster : public CCNode {
public:
    LevelEditorLayer* editor = nullptr;

    std::vector<std::string> chunks;

    size_t currentChunk = 0;
    int totalObjects = 0;

    Notification* notification = nullptr;

    static DecorPaster* create(
        LevelEditorLayer* target,
        std::vector<std::string> data,
        int total,
        Notification* notif
    ) {
        auto* result = new DecorPaster();

        if (result && result->init()) {
            result->editor = target;
            result->chunks = std::move(data);
            result->totalObjects = total;
            result->notification = notif;

            result->autorelease();

            return result;
        }

        delete result;
        return nullptr;
    }

    void update(float) override {
        if (!editor) {
            unscheduleUpdate();
            removeFromParentAndCleanup(true);
            return;
        }

        if (currentChunk >= chunks.size()) {
            unscheduleUpdate();

            if (notification) notification->hide();

            Notification::create(
                "Decoracao aplicada: " +
                std::to_string(totalObjects) +
                " objetos. Salve sua fase!",
                NotificationIcon::Success,
                5.f
            )->show();

            removeFromParentAndCleanup(true);
            return;
        }

        editor->createObjectsFromString(
            chunks[currentChunk],
            true,
            true
        );

        currentChunk++;

        if (notification) {
            notification->setString(
                "Colando " +
                std::to_string(currentChunk) +
                "/" +
                std::to_string(chunks.size())
            );
        }
    }
};

// FIM DA PARTE 1

// ============================================================
// DECOR COPY V2
// Parte 2 - agrupamento, aplicacao, download e interface
// ============================================================

// ------------------------------------------------------------
// Configuracao e processamento da fase de referencia
// ------------------------------------------------------------

static TaskHolder<web::WebResponse> s_holder;

static void showError(std::string const& message) {
    Notification::create(
        message,
        NotificationIcon::Error,
        5.f
    )->show();
}

static void processLevel(
    LevelEditorLayer* editor,
    std::string const& decoded
) {
    if (!editor) {
        showError("Editor nao encontrado");
        return;
    }

    auto mod = Mod::get();

    bool themeOnly =
        mod->getSettingValue<bool>("theme-only");

    bool copyColors =
        mod->getSettingValue<bool>("copy-colors");

    bool copyBackground =
        mod->getSettingValue<bool>("copy-bg");

    bool copySpikes =
        mod->getSettingValue<bool>("pattern-spikes");

    bool copyBlocks =
        mod->getSettingValue<bool>("pattern-blocks");

    int zorder = static_cast<int>(
        mod->getSettingValue<int64_t>("overlay-zorder")
    );

    float radius = static_cast<float>(
        mod->getSettingValue<int64_t>("radius")
    );

    size_t maxPerObject = static_cast<size_t>(
        mod->getSettingValue<int64_t>("max-per-object")
    );

    size_t maxObjects = static_cast<size_t>(
        mod->getSettingValue<int64_t>("max-objects")
    );

    size_t chunkSize = static_cast<size_t>(
        std::max<int64_t>(
            20,
            mod->getSettingValue<int64_t>("chunk-size")
        )
    );

    auto segments = splitStr(decoded, ';');

    std::string header =
        segments.empty() ? "" : segments[0];

    if (themeOnly) {
        applyHeader(
            editor,
            header,
            copyColors,
            copyBackground
        );

        Notification::create(
            "Tema aplicado! Salve sua fase.",
            NotificationIcon::Success,
            5.f
        )->show();

        return;
    }

    if (radius <= 0.f) radius = 90.f;
    if (maxPerObject == 0) maxPerObject = 300;
    if (maxObjects == 0) maxObjects = 40000;

    // --------------------------------------------------------
    // 1. Ler todos os objetos da fase original
    // --------------------------------------------------------

    std::vector<LevelObject> sourceObjects;
    sourceObjects.reserve(segments.size());

    for (size_t i = 1; i < segments.size(); ++i) {
        if (segments[i].empty()) continue;

        LevelObject object;

        if (!parseObject(segments[i], object)) {
            continue;
        }

        object.kind = classifyObject(object.id);

        sourceObjects.push_back(std::move(object));
    }

    if (sourceObjects.empty()) {
        showError("A fase original esta vazia");
        return;
    }

    // --------------------------------------------------------
    // 2. Encontrar os objetos que receberao decoracoes
    // --------------------------------------------------------

    std::vector<SpatialAnchor> anchors;
    std::vector<int> decorationIndices;

    for (
        int i = 0;
        i < static_cast<int>(sourceObjects.size());
        ++i
    ) {
        auto const& object = sourceObjects[i];

        if (object.trigger) continue;

        if (
            (object.kind == 1 && copyBlocks) ||
            (object.kind == 2 && copySpikes)
        ) {
            SpatialAnchor anchor;

            anchor.objectIndex = i;
            anchor.id = object.id;
            anchor.kind = object.kind;
            anchor.x = object.x;
            anchor.y = object.y;
            anchor.rotation = object.rotation;
            anchor.flipY = object.flipY;

            anchors.push_back(anchor);
        }
        else if (object.kind == 3) {
            decorationIndices.push_back(i);
        }
    }

    if (anchors.empty()) {
        applyHeader(
            editor,
            header,
            copyColors,
            copyBackground
        );

        showError("Nenhum bloco ou espinho encontrado na referencia");
        return;
    }

    // --------------------------------------------------------
    // 3. Criar grade espacial dos objetos principais
    // --------------------------------------------------------

    SpatialGrid grid;
    grid.cellSize = radius;

    for (
        int i = 0;
        i < static_cast<int>(anchors.size());
        ++i
    ) {
        grid.insert(
            anchors[i].x,
            anchors[i].y,
            i
        );
    }

    // Cada decoracao sera associada ao anchor mais proximo.
    std::vector<std::vector<int>> groupedDecorations(
        anchors.size()
    );

    for (int decorationIndex : decorationIndices) {
        auto const& decoration =
            sourceObjects[decorationIndex];

        auto nearby = grid.nearby(
            decoration.x,
            decoration.y
        );

        float bestDistance =
            radius * radius;

        int bestAnchor = -1;

        for (int anchorIndex : nearby) {
            auto const& anchor = anchors[anchorIndex];

            float dx = decoration.x - anchor.x;
            float dy = decoration.y - anchor.y;

            float distance = dx * dx + dy * dy;

            if (distance <= bestDistance) {
                bestDistance = distance;
                bestAnchor = anchorIndex;
            }
        }

        if (bestAnchor >= 0) {
            groupedDecorations[bestAnchor].push_back(
                decorationIndex
            );
        }
    }

    // --------------------------------------------------------
    // 4. Montar todos os padroes encontrados
    // --------------------------------------------------------

    std::map<int, std::vector<DecorationPattern>> patternsByID;

    std::map<int, std::vector<DecorationPattern>> patternsByKind;

    for (
        int i = 0;
        i < static_cast<int>(anchors.size());
        ++i
    ) {
        auto const& anchor = anchors[i];

        auto const& source =
            sourceObjects[anchor.objectIndex];

        auto const& decorations =
            groupedDecorations[i];

        if (decorations.empty()) continue;

        DecorationPattern pattern;

        pattern.anchorID = anchor.id;
        pattern.anchorKind = anchor.kind;

        float anchorRotation = anchor.rotation;

        if (anchor.flipY) {
            anchorRotation += 180.f;
        }

        size_t count = std::min(
            decorations.size(),
            maxPerObject
        );

        for (size_t j = 0; j < count; ++j) {
            auto const& decoration =
                sourceObjects[decorations[j]];

            DecorationPiece piece;

            piece.raw = cleanDecoration(
                decoration.raw
            );

            rotatePoint(
                decoration.x - anchor.x,
                decoration.y - anchor.y,
                -anchorRotation,
                piece.localX,
                piece.localY
            );

            piece.relativeRotation =
                decoration.rotation - anchorRotation;

            pattern.pieces.push_back(
                std::move(piece)
            );
        }

        if (pattern.pieces.empty()) continue;

        patternsByID[anchor.id].push_back(pattern);

        patternsByKind[anchor.kind].push_back(
            std::move(pattern)
        );
    }

    if (patternsByID.empty()) {
        applyHeader(
            editor,
            header,
            copyColors,
            copyBackground
        );

        showError(
            "Nao encontrei decoracoes proximas dos objetos"
        );

        return;
    }

    // --------------------------------------------------------
    // 5. Ler os objetos da sua fase
    // --------------------------------------------------------

    CCArray* objects = editor->m_objects;

    std::vector<GameObject*> targetObjects;

    if (objects) {
        for (int i = 0; i < objects->count(); ++i) {
            auto* object =
                static_cast<GameObject*>(
                    objects->objectAtIndex(i)
                );

            if (object) {
                targetObjects.push_back(object);
            }
        }
    }

    if (targetObjects.empty()) {
        showError("Sua fase esta vazia");
        return;
    }

    // --------------------------------------------------------
    // 6. Aplicar os padroes aos objetos equivalentes
    // --------------------------------------------------------

    std::vector<std::string> generated;

    generated.reserve(4096);

    std::unordered_map<int, size_t> counters;

    size_t decoratedObjects = 0;

    for (auto* target : targetObjects) {
        if (generated.size() >= maxObjects) {
            break;
        }

        int id = target->m_objectID;

        int kind = classifyObject(id);

        if (kind != 1 && kind != 2) {
            continue;
        }

        if (kind == 1 && !copyBlocks) continue;
        if (kind == 2 && !copySpikes) continue;

        std::vector<DecorationPattern>* selected = nullptr;

        auto exact = patternsByID.find(id);

        if (
            exact != patternsByID.end() &&
            !exact->second.empty()
        ) {
            selected = &exact->second;
        }
        else {
            auto fallback = patternsByKind.find(kind);

            if (
                fallback != patternsByKind.end() &&
                !fallback->second.empty()
            ) {
                selected = &fallback->second;
            }
        }

        if (!selected || selected->empty()) {
            continue;
        }

        size_t patternIndex =
            counters[id]++ % selected->size();

        auto const& pattern =
            (*selected)[patternIndex];

        float targetX = target->getPositionX();
        float targetY = target->getPositionY();
        float targetRotation = target->getRotation();

        size_t added = 0;

        for (auto const& piece : pattern.pieces) {
            if (generated.size() >= maxObjects) {
                break;
            }

            if (added >= maxPerObject) {
                break;
            }

            float offsetX = 0.f;
            float offsetY = 0.f;

            rotatePoint(
                piece.localX,
                piece.localY,
                targetRotation,
                offsetX,
                offsetY
            );

            std::string raw = piece.raw;

            raw = setKey(
                raw,
                2,
                numStr(targetX + offsetX)
            );

            raw = setKey(
                raw,
                3,
                numStr(targetY + offsetY)
            );

            raw = setKey(
                raw,
                6,
                numStr(
                    targetRotation +
                    piece.relativeRotation
                )
            );

            raw = setKey(
                raw,
                25,
                std::to_string(zorder)
            );

            generated.push_back(std::move(raw));

            added++;
        }

        if (added > 0) {
            decoratedObjects++;
        }
    }

    // --------------------------------------------------------
    // 7. Aplicar cores e fundo
    // --------------------------------------------------------

    applyHeader(
        editor,
        header,
        copyColors,
        copyBackground
    );

    if (generated.empty()) {
        showError(
            "Nenhuma decoracao compativel encontrada"
        );
        return;
    }

    // --------------------------------------------------------
    // 8. Separar os objetos em partes
    // --------------------------------------------------------

    std::vector<std::string> chunks;

    std::string current;
    size_t count = 0;

    for (auto const& object : generated) {
        current += object;
        current += ';';

        count++;

        if (count >= chunkSize) {
            chunks.push_back(current);
            current.clear();
            count = 0;
        }
    }

    if (!current.empty()) {
        chunks.push_back(current);
    }

    auto notification = Notification::create(
        "Preparando decoracoes...",
        NotificationIcon::Loading,
        0.f
    );

    notification->show();

    auto paster = DecorPaster::create(
        editor,
        std::move(chunks),
        static_cast<int>(generated.size()),
        notification
    );

    if (!paster) {
        notification->hide();
        showError("Erro ao preparar a colagem");
        return;
    }

    editor->addChild(paster);
    paster->scheduleUpdate();
}

// ------------------------------------------------------------
// 9. Baixar fase pelo servidor oficial
// ------------------------------------------------------------

static void startImport(
    Ref<LevelEditorLayer> editor,
    int levelID
) {
    if (levelID <= 0) {
        showError("ID invalido");
        return;
    }

    Notification::create(
        "Baixando fase " + std::to_string(levelID) + "...",
        NotificationIcon::Loading,
        2.f
    )->show();

    web::WebRequest request;

    request.userAgent("");

    request.header(
        "Content-Type",
        "application/x-www-form-urlencoded"
    );

    request.bodyString(
        "levelID=" +
        std::to_string(levelID) +
        "&secret=Wmfd2893gb7"
    );

    s_holder.spawn(
        request.post(
            "https://www.boomlings.com/database/downloadGJLevel22.php"
        ),
        [editor](web::WebResponse response) {
            if (!editor || !editor->getParent()) {
                return;
            }

            if (!response.ok()) {
                showError(
                    "Falha no download. Codigo " +
                    std::to_string(response.code())
                );
                return;
            }

            std::string responseText =
                response.string().unwrapOr("");

            auto hashPosition =
                responseText.find('#');

            if (hashPosition != std::string::npos) {
                responseText =
                    responseText.substr(0, hashPosition);
            }

            auto fields = splitStr(responseText, ':');

            std::string rawLevel;

            for (
                size_t i = 0;
                i + 1 < fields.size();
                i += 2
            ) {
                if (fields[i] == "4") {
                    rawLevel = fields[i + 1];
                    break;
                }
            }

            if (rawLevel.empty()) {
                showError("Fase nao encontrada ou privada");
                return;
            }

            std::string decoded;

            if (rawLevel.rfind("kS", 0) == 0) {
                decoded = rawLevel;
            }
            else {
                gd::string decompressed =
                    ZipUtils::decompressString(
                        gd::string(rawLevel),
                        false,
                        11
                    );

                decoded = decompressed.c_str();
            }

            if (decoded.empty()) {
                showError(
                    "Nao foi possivel decodificar a fase"
                );
                return;
            }

            processLevel(
                editor,
                decoded
            );
        }
    );
}

// ------------------------------------------------------------
// 10. Botao no menu de pausa do editor
// ------------------------------------------------------------

class $modify(DecorPauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorPauseLayer::init(editor)) {
            return false;
        }

        auto win = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();

        auto sprite = ButtonSprite::create(
            "Copiar decor"
        );

        sprite->setScale(0.7f);

        auto button = CCMenuItemSpriteExtra::create(
            sprite,
            this,
            menu_selector(DecorPauseLayer::onCopyDecor)
        );

        menu->addChild(button);

        button->setPosition({0.f, 0.f});

        menu->setPosition({
            win.width - 65.f,
            win.height - 25.f
        });

        this->addChild(menu, 100);

        return true;
    }

    void onCopyDecor(CCObject*) {
        auto holder =
            std::make_shared<TextInput*>(nullptr);

        auto alert = createQuickPopup(
            "Decor Copy V2",
            "Digite o ID da fase de referencia:\n\n\n",
            "Cancelar",
            "Copiar",
            [
                editor = Ref<LevelEditorLayer>(m_editorLayer),
                holder
            ](FLAlertLayer*, bool confirmed) {
                if (!confirmed) return;

                int id = 0;

                if (*holder) {
                    id = toInt((*holder)->getString());
                }

                if (id <= 0) {
                    id = static_cast<int>(
                        Mod::get()->getSettingValue<int64_t>(
                            "level-id"
                        )
                    );
                }

                startImport(editor, id);
            },
            false
        );

        auto win = CCDirector::get()->getWinSize();

        auto input = TextInput::create(
            160.f,
            "ID da fase",
            "bigFont.fnt"
        );

        input->setFilter("0123456789");
        input->setMaxCharCount(10);

        input->setPosition({
            win.width / 2.f,
            win.height / 2.f - 8.f
        });

        alert->m_mainLayer->addChild(input);

        *holder = input;

        alert->show();
    }
};

// FIM DO MAIN.CPP
