// Minixx's Orb Buffer — par Minixx
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <sstream>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

// ---------------------------------------------------------------- état global
enum class Mode { Off = 0, Record = 1, Assist = 2 };

struct Click { int frame; bool down; bool p1; };
struct Macro {
    std::vector<Click> clicks;
    std::vector<CCPoint> trace; // position du joueur 1, index = frame
};

static Macro g_macro;
static Mode g_mode = Mode::Off;
static bool g_sim = false;      // simulation interne en cours
static bool g_firing = false;   // clic différé en cours d'envoi
static int g_pendingDown = -1;  // frame où envoyer le clic différé
static int g_pendingUp = -1;    // frame où envoyer le relâchement différé
static bool g_releaseQueued = false;

constexpr float SIM_DT = 1.f / 240.f; // 1 pas de physique

static int frameOf(GJBaseGameLayer* l) { return l->m_gameState.m_currentProgress; }

static std::string macroKey() {
    int id = 0;
    if (auto pl = PlayLayer::get()) id = pl->m_level->m_levelID.value();
    return fmt::format("macro_{}", id);
}

// La macro est stockée dans les "saved values" du mod (pas de fichier -> compatible partout)
static void saveMacro() {
    std::ostringstream o;
    for (auto& c : g_macro.clicks) o << "C " << c.frame << ' ' << (int)c.down << ' ' << (int)c.p1 << '\n';
    for (size_t i = 0; i < g_macro.trace.size(); i++)
        o << "P " << i << ' ' << g_macro.trace[i].x << ' ' << g_macro.trace[i].y << '\n';
    Mod::get()->setSavedValue<std::string>(macroKey(), o.str());
}

static bool loadMacro() {
    auto data = Mod::get()->getSavedValue<std::string>(macroKey(), "");
    if (data.empty()) return false;
    std::istringstream f(data);
    g_macro = {};
    std::string t;
    while (f >> t) {
        if (t == "C") {
            Click c{}; int d, p;
            f >> c.frame >> d >> p;
            c.down = d != 0; c.p1 = p != 0;
            g_macro.clicks.push_back(c);
        }
        else if (t == "P") {
            size_t i; float x, y;
            f >> i >> x >> y;
            if (g_macro.trace.size() <= i) g_macro.trace.resize(i + 1);
            g_macro.trace[i] = CCPoint(x, y);
        }
    }
    return !g_macro.clicks.empty();
}

// ------------------------------------------------- calcul de la trajectoire / du délai
// PlayLayer::loadFromCheckpoint n'existe pas sur iOS (symbole introuvable à l'édition de
// liens), donc pas de simulation physique : on prédit l'arrivée au point de clic de la macro.
// Le joueur avance à vitesse (quasi) constante en X : le nombre de frames avant d'atteindre
// la position du clic enregistré = distance restante / vitesse par frame (relevée dans la macro).
static int chooseDelay(PlayLayer* pl, int f) {
    int maxDelay = (int)Mod::get()->getSettingValue<int64_t>("max-delay");
    int n = (int)g_macro.trace.size();
    if (n < 3) return 0;

    float px = pl->m_player1->getPositionX();

    // prochain clic de la macro encore devant le joueur
    const Click* ref = nullptr;
    float bestDx = 1e9f;
    for (auto& c : g_macro.clicks) {
        if (!c.down || !c.p1 || c.frame < 1 || c.frame >= n - 1) continue;
        float dx = g_macro.trace[c.frame].x - px;
        if (dx < -1.f) continue; // déjà dépassé
        if (dx < bestDx) { bestDx = dx; ref = &c; }
    }
    if (!ref) return 0;

    // vitesse horizontale (par frame) autour du clic, d'après la macro
    float speed = (g_macro.trace[ref->frame + 1].x - g_macro.trace[ref->frame - 1].x) / 2.f;
    if (speed <= 0.01f) return 0;

    int delay = (int)std::lround(bestDx / speed);
    if (delay <= 0 || delay > maxDelay) return 0; // pas près de l'orbe : clic normal
    return delay;
}

// ------------------------------------------------------------------------- hooks
class $modify(OBBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (!pl || g_sim || g_firing || g_mode == Mode::Off ||
            !Mod::get()->getSettingValue<bool>("enabled")) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        int f = frameOf(this);

        if (g_mode == Mode::Record) {
            g_macro.clicks.push_back({f, down, isPlayer1});
        }
        else if (g_mode == Mode::Assist && button == 1 && isPlayer1) {
            if (down && !g_macro.clicks.empty()) {
                int d = chooseDelay(pl, f);
                if (d > 0) { g_pendingDown = f + d; return; } // clic retardé
            }
            else if (!down && g_pendingDown >= 0) {
                g_releaseQueued = true; // relâche après le clic retardé
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(OBPlayLayer, PlayLayer) {
#if GEODE_COMP_GD_VERSION >= 22091
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
#else
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
#endif
        // nouvel essai de niveau : on repart propre (évite une macro d'un autre niveau)
        g_macro = {};
        g_mode = Mode::Off;
        g_sim = g_firing = g_releaseQueued = false;
        g_pendingDown = g_pendingUp = -1;
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_pendingDown = g_pendingUp = -1;
        g_releaseQueued = false;
        if (g_mode == Mode::Record) { g_macro.clicks.clear(); g_macro.trace.clear(); }
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (g_sim) return;
        int f = frameOf(this);

        if (g_mode == Mode::Record) {
            if ((int)g_macro.trace.size() <= f) g_macro.trace.resize(f + 1);
            g_macro.trace[f] = m_player1->getPosition();
        }
        if (g_mode == Mode::Assist) {
            if (g_pendingDown >= 0 && f >= g_pendingDown) {
                g_firing = true;
                this->handleButton(true, 1, true);
                g_firing = false;
                g_pendingDown = -1;
                if (g_releaseQueued) { g_pendingUp = f + 1; g_releaseQueued = false; }
            }
            if (g_pendingUp >= 0 && f >= g_pendingUp) {
                g_firing = true;
                this->handleButton(false, 1, true);
                g_firing = false;
                g_pendingUp = -1;
            }
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (g_mode == Mode::Record) saveMacro();
    }
};

// --------------------------------------------- UI : terminal de hacker (noir / rouge)
static const ccColor3B RED = {255, 59, 31};
static const ccColor3B DIM = {150, 40, 25};

static CCLabelBMFont* termLabel(const char* text, float scale, ccColor3B col = RED) {
    auto l = CCLabelBMFont::create(text, "chatFont.fnt");
    l->setColor(col);
    l->setScale(scale);
    return l;
}

// bouton "boîte noire à bordure rouge"
static CCMenuItemSpriteExtra* termButton(const char* text, float w, CCObject* target, SEL_MenuHandler cb) {
    auto border = CCScale9Sprite::create("square02_001.png");
    border->setContentSize({w, 28});
    border->setColor(RED);
    border->setOpacity(255);
    auto inner = CCScale9Sprite::create("square02_001.png");
    inner->setContentSize({w - 4, 24});
    inner->setColor({0, 0, 0});
    inner->setOpacity(255);
    inner->setPosition(border->getContentSize() / 2);
    border->addChild(inner);
    auto label = termLabel(text, 0.6f);
    float maxW = w - 10;
    if (label->getContentSize().width * label->getScale() > maxW)
        label->setScale(maxW / label->getContentSize().width);
    label->setPosition(border->getContentSize() / 2);
    border->addChild(label, 2);
    return CCMenuItemSpriteExtra::create(border, nullptr, target, cb);
}

class OBPopup : public geode::Popup {
protected:
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_prompt = nullptr;
    std::string m_msg = "pret.";
    bool m_cursor = true;

    bool init() {
        if (!Popup::init(340.f, 230.f)) return false;
        m_bgSprite->setVisible(false);
        CCPoint c = m_mainLayer->getContentSize() / 2; // centre réel du popup

        auto border = CCLayerColor::create({RED.r, RED.g, RED.b, 255}, 344, 234);
        border->setPosition(c.x - 172, c.y - 117);
        m_mainLayer->addChild(border, -2);
        auto bg = CCLayerColor::create({0, 0, 0, 245}, 340, 230);
        bg->setPosition(c.x - 170, c.y - 115);
        m_mainLayer->addChild(bg, -1);

        auto add = [&](CCLabelBMFont* l, float x, float y, bool left = false) {
            if (left) l->setAnchorPoint({0, 0.5f});
            l->setPosition(c.x + x, c.y + y);
            m_mainLayer->addChild(l);
        };

        add(termLabel("MINIXX'S ORB BUFFER", 1.0f), 0, 98);
        add(termLabel("Version 1.0  |  par Minixx", 0.55f), 0, 80);
        add(termLabel("======================================", 0.6f), 0, 66);
        add(termLabel("User : OPERATOR", 0.6f), -158, 52, true);
        m_status = termLabel("", 0.6f);
        add(m_status, -158, 38, true);
        add(termLabel("Commands :", 0.6f), -158, 22, true);

        struct Cmd { const char* name; const char* desc; int tag; };
        Cmd cmds[] = {
            {"record", "-> Enregistrer la macro",    1},
            {"assist", "-> Activer le buffer d'orbes", 2},
            {"off",    "-> Desactiver",              0},
            {"save",   "-> Sauvegarder la macro",    3},
            {"load",   "-> Charger la macro",        4},
            {"exit",   "-> Quitter",                 5},
        };

        auto menu = CCMenu::create();
        menu->setPosition(c);
        menu->setContentSize({0, 0});
        m_mainLayer->addChild(menu);

        float y = 6;
        for (auto& cmd : cmds) {
            auto lbl = termLabel(cmd.name, 0.6f);
            auto item = CCMenuItemSpriteExtra::create(lbl, nullptr, this, menu_selector(OBPopup::onCmd));
            item->setTag(cmd.tag);
            item->setPosition(-148 + lbl->getContentSize().width * 0.3f, y);
            menu->addChild(item);
            add(termLabel(cmd.desc, 0.6f), -80, y, true);
            y -= 15;
        }

        add(termLabel("======================================", 0.6f), 0, -92);
        m_prompt = termLabel("", 0.6f);
        add(m_prompt, -158, -105, true);

        this->schedule(schedule_selector(OBPopup::blink), 0.5f);
        refresh();
        return true;
    }

    void blink(float) { m_cursor = !m_cursor; refresh(); }

    void say(std::string msg) { m_msg = std::move(msg); refresh(); }

    void refresh() {
        const char* st[] = {"OFFLINE", "RECORDING", "ASSIST ONLINE"};
        m_status->setString(fmt::format("Status : {}   (clics : {})", st[(int)g_mode], g_macro.clicks.size()).c_str());
        m_prompt->setString(fmt::format("ORBBUF > {}{}", m_msg, m_cursor ? "_" : " ").c_str());
    }

    void onCmd(CCObject* s) {
        switch (static_cast<CCNode*>(s)->getTag()) {
            case 0:
                if (g_mode == Mode::Record) saveMacro();
                g_mode = Mode::Off; say("buffer desactive.");
                break;
            case 1:
                g_mode = Mode::Record; say("record : joue le niveau.");
                break;
            case 2:
                if (g_mode == Mode::Record) saveMacro();
                if (g_macro.clicks.empty() && !loadMacro()) { say("erreur : aucune macro, fais record."); break; }
                g_mode = Mode::Assist; say("assist actif.");
                break;
            case 3: saveMacro(); say("macro sauvegardee."); break;
            case 4: say(loadMacro() ? "macro chargee." : "erreur : aucune macro sauvegardee."); break;
            default: this->keyBackClicked(); return;
        }
        refresh();
    }

public:
    static OBPopup* create() {
        auto ret = new OBPopup();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

class $modify(OBPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto ws = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition(ws.width - 75, 25);
        menu->addChild(termButton("> ORB BUFFER", 120, this, menu_selector(OBPause::onOrbBuffer)));
        this->addChild(menu);
    }
    void onOrbBuffer(CCObject*) { OBPopup::create()->show(); }
};
