// Predictive Input Buffer v2
//
// When you click (or release, in wave / robot), a hidden "ghost" player is simulated
// ahead with your input. If the ghost dies, the input is retried with 1..N ticks of
// delay and, if a delayed version clearly survives longer, your input is held back
// and delivered that many physics ticks later. 1 tick = 1/240 s = 0.25 frames @ 60fps.
//
// v2 changes:
//  - Bigger windows: F6 stages up to 32 ticks (8 frames) plus a CUSTOM stage (settings).
//  - Smart prediction: ghost uses orbs, pads, gravity portals and speed portals; mode /
//    size / dual / mirror / teleport portals end the prediction as "safe".
//  - Input queue: every input arriving during a delay is shifted by the same amount,
//    so taps and hold lengths are preserved (no more swallowed clicks / 1-tick taps).
//  - Time budget per click so big windows don't hitch.
//  - Fixes: ghost lifetime, init using `this`, time-warp dt, F6 only when GD focused,
//    dead flipGravity hook priority, optional console, on-screen stage label.

#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    // Objects the ghost always collides with.
    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    // Objects the ghost may also interact with when smart prediction is on.
    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    // Portals that change too much state to simulate: reaching one ends prediction as "safe".
    constexpr uint64_t STOP_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal) |
        typeBit(GameObjectType::InverseMirrorPortal) | typeBit(GameObjectType::NormalMirrorPortal) |
        typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    bool hasType(uint64_t mask, GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 && ((mask >> type) & 1);
    }

    float speedPortalValue(int id) {
        switch (id) {
            case 200: return 0.7f;   // slow
            case 201: return 0.9f;   // normal
            case 202: return 1.1f;   // fast
            case 203: return 1.3f;   // faster
            case 1334: return 1.6f;  // fastest
            default: return 0.f;
        }
    }

    struct Stage {
        bool enabled;
        int ticksAll;
        int ticksWave;
        bool custom;
    };

    constexpr Stage STAGES[] = {
        { true, 2, 2, false },
        { true, 4, 4, false },
        { true, 8, 8, false },
        { true, 16, 8, false },
        { true, 24, 12, false },
        { true, 32, 16, false },
        { true, 0, 0, true },
        { false, 0, 0, false },
    };
    constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

    struct QueuedInput {
        int ticksLeft;
        bool down;
    };

    struct ObjSnapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
    };

    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLabelBMFont* label = nullptr;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;

        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;

        std::deque<QueuedInput> queue;
        int shift = 0;  // delay applied to inputs while the queue is active

        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;

        bool smart = true;
        bool console = false;
        double budgetMs = 6.0;

        std::unordered_set<GameObject*> simUsed;
        std::vector<ObjSnapshot> snapshots;
    } buf;

    template <class... Args>
    void say(fmt::format_string<Args...> f, Args&&... args) {
        auto msg = fmt::format(f, std::forward<Args>(args)...);
        if (buf.console) {
            std::printf("[InputBuffer] %s\n", msg.c_str());
        }
        log::info("{}", msg);
    }

    bool isGhost(PlayerObject* p) {
        return p && (p == buf.ghost1 || p == buf.ghost2);
    }

    bool gdFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    void resetPending() {
        buf.queue.clear();
        buf.shift = 0;
    }

    void readSettings() {
        auto* mod = Mod::get();
        buf.smart = mod->getSettingValue<bool>("smart-prediction");
        buf.budgetMs = mod->getSettingValue<double>("sim-budget-ms");
    }

    void applyStage() {
        auto const& s = STAGES[buf.stage];
        buf.enabled = s.enabled;
        if (s.custom) {
            buf.ticksAll = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-ticks"));
            buf.ticksWave = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-wave-ticks"));
        }
        else {
            buf.ticksAll = s.ticksAll;
            buf.ticksWave = s.ticksWave;
        }
    }

    std::string stageText() {
        if (!buf.enabled) {
            return "Buffer: OFF";
        }
        return fmt::format("Buffer{}: {:g}f / wave {:g}f", STAGES[buf.stage].custom ? " (custom)" : "", buf.ticksAll * TICK,
            buf.ticksWave * TICK);
    }

    void updateLabel() {
        if (!buf.label) {
            return;
        }
        buf.label->setVisible(Mod::get()->getSettingValue<bool>("show-indicator"));
        buf.label->setString(stageText().c_str());
    }

    void dropGhosts() {
        for (auto** g : { &buf.ghost1, &buf.ghost2 }) {
            if (*g) {
                (*g)->removeFromParentAndCleanup(true);
                (*g)->release();
                *g = nullptr;
            }
        }
        buf.label = nullptr;  // owned by the PlayLayer
        buf.simulating = false;
        buf.injecting = false;
        buf.rapid = 0;
        resetPending();
    }

    PlayerObject* makeGhost(PlayLayer* layer) {
        auto* ghost = PlayerObject::create(1, 1, layer, layer, true);
        ghost->retain();
        ghost->setPosition(CCPoint(0.f, 105.f));
        ghost->setVisible(false);
        layer->m_objectLayer->addChild(ghost);
        return ghost;
    }

    void sendButton(PlayLayer* layer, bool down) {
        buf.injecting = true;
        buf.appliedInput = down;
        layer->handleButton(down, 1, buf.isPlayer1);
        buf.injecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        if (down) {
            ghost->pushButton(PlayerButton::Jump);
        }
        else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;

        // Button / jump-buffer state. Without this the ghost keeps whatever button
        // state the PREVIOUS simulation left it in (usually "held"), so on some clicks
        // its pushButton() is a no-op or it auto-jumps from hold, and the prediction
        // flips between right and wrong from click to click.
        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
        ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump;
        ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump;
        ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad;
        ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;
    }

    // How long the player usually holds a click, so cube/ball/ufo/spider/swing
    // predictions model a tap instead of an infinite hold (holding auto-re-jumps).
    constexpr int HOLD_SAMPLES = 8;
    int holdSamples[HOLD_SAMPLES] = {};
    int holdCount = 0;
    int holdIndex = 0;
    int tickCount = 0;
    int pressTick = -1;

    void recordRealInput(bool down) {
        if (down) {
            pressTick = tickCount;
            return;
        }
        if (pressTick < 0) {
            return;
        }
        int held = std::clamp(tickCount - pressTick, 1, 240);
        pressTick = -1;
        holdSamples[holdIndex] = held;
        holdIndex = (holdIndex + 1) % HOLD_SAMPLES;
        holdCount = std::min(holdCount + 1, HOLD_SAMPLES);
    }

    int holdEstimate() {
        if (holdCount == 0) {
            return 12;  // ~50 ms, a typical tap
        }
        int tmp[HOLD_SAMPLES];
        std::copy(holdSamples, holdSamples + holdCount, tmp);
        std::nth_element(tmp, tmp + holdCount / 2, tmp + holdCount);
        return tmp[holdCount / 2];
    }

    void snapshotObject(GameObject* object) {
        if (!object || buf.simUsed.contains(object)) {
            return;
        }
        buf.simUsed.insert(object);
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
        }
        buf.snapshots.push_back(s);
    }

    void restoreObjects() {
        for (auto const& s : buf.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
                s.effect->m_activatedByPlayer1 = s.byP1;
                s.effect->m_activatedByPlayer2 = s.byP2;
            }
        }
        buf.snapshots.clear();
        buf.simUsed.clear();
    }

    void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }

    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing;
    }

    enum class SimEnd { Ok, StopSearch };

    // Returns ticks survived after the input (horizon = fully safe).
    // releaseAfter: ticks after the press at which the ghost lets go (-1 = hold).
    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        SimEnd* end) {
        *end = SimEnd::Ok;
        if (!buf.ghost1) {
            return horizon;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.ghostHitPortal = false;

        bool second = dual && buf.ghost2 && p2;
        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }

        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }

        // (v1 forced a fake !down button event here for delay > 0, which made delayed
        //  runs start from a different jump-buffer state than the "press now" run.
        //  The ghost now simply mirrors the real player's button state.)

        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
        int total = delay + horizon;
        int survived = 0;
        int airTicks = 0;
        int groundTicks = 0;
        int tick = 0;
        bool died = false;
        bool safe = false;

        for (; tick < total; ++tick) {
            if (tick == delay) {
                pressGhost(buf.ghost1, down);
                if (second) {
                    pressGhost(buf.ghost2, down);
                }
            }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) {
                pressGhost(buf.ghost1, false);
                if (second) {
                    pressGhost(buf.ghost2, false);
                }
            }

            advanceGhost(buf.ghost1, dt);
            if (second) {
                advanceGhost(buf.ghost2, dt);
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }

            checkGhost(layer, buf.ghost1, dt);
            if (!buf.ghostDied && second) {
                checkGhost(layer, buf.ghost2, dt);
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }
            if (buf.ghostHitPortal) {
                safe = tick >= delay;
                break;
            }

            if (tick < delay) {
                continue;
            }
            ++survived;
            if (flying) {
                continue;
            }

            bool grounded = buf.ghost1->m_isOnGround && (!second || buf.ghost2->m_isOnGround);
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            if (airTicks < 6) {
                continue;
            }
            if (groundTicks > 2) {
                safe = true;
                break;
            }
            ++groundTicks;
        }

        // Died or hit an unsimulated portal before the delayed input even fired:
        // longer delays can't do better, stop searching.
        if ((died || buf.ghostHitPortal) && tick < delay) {
            *end = SimEnd::StopSearch;
        }
        restoreObjects();
        buf.simulating = false;
        return safe ? horizon : survived;
    }

    // Find the smallest delay that clearly beats pressing now. 0 = don't buffer.
    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, bool anyWave, int horizon, int maxDelay,
        int releaseAfter, int base, int* landed) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs;
        };

        int target = anyWave ? base + 10 : std::min(horizon, base + 12);
        int gain = 6;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = simulate(layer, p1, p2, dual, k, down, horizon, releaseAfter, &end);
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            bool good;
            if (anyWave) {
                good = ret < horizon ? ret >= target : (ret - base >= gain || ret >= target);
                target += 2;
                gain += 2;
            }
            else {
                good = ret >= target;
            }
            if (good) {
                *landed = ret;
                return k;
            }
            if (overBudget()) {
                say("prediction budget hit at +{} ticks", k);
                return 0;
            }
        }
        return 0;
    }

    void queueInput(int ticks, bool down) {
        buf.queue.push_back({ ticks, down });
    }

    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return false;
        }

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* level = layer->m_level;
        if (level && !isPlayer1 && level->m_twoPlayerMode) {
            return false;
        }

        buf.isPlayer1 = isPlayer1;
        buf.realInput = down;
        if (!buf.enabled) {
            buf.appliedInput = down;
            return false;
        }

        auto* p1 = self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        if (!p1 || p1->m_isDead || p1->m_isShip || (p2 && p2->m_isShip)) {
            resetPending();
            buf.appliedInput = down;
            return false;
        }

        // A delay is in progress: shift this input by the same amount so your
        // rhythm (tap length, hold length, follow-up clicks) is preserved.
        if (!buf.queue.empty()) {
            int after = buf.queue.back().ticksLeft;
            queueInput(std::max(buf.shift, after), down);
            buf.sinceInput = 0;
            return true;
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        // Wave spam: don't fight fast inputs.
        if (buf.sinceInput < 8) {
            int before = buf.rapid++;
            buf.sinceInput = 0;
            if (before >= 2 && anyWave) {
                buf.appliedInput = down;
                return false;
            }
        }
        else {
            buf.sinceInput = 0;
            buf.rapid = 0;
        }

        // Releases only matter for wave and robot.
        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) {
                wave2 = p2->m_isDart;
                if (!wave2) {
                    robot = p2->m_isRobot;
                }
            }
            if (!wave2 && !down && !robot) {
                buf.appliedInput = false;
                return false;
            }
        }

        int horizon = mini ? 28 : (anyWave ? 38 : 48);
        int maxDelay = anyWave ? buf.ticksWave : buf.ticksAll;
        if (maxDelay <= 0) {
            buf.appliedInput = down;
            return false;
        }

        // Wave and robot releases are predicted separately, so hold there; everything
        // else (cube, ball, ufo, spider, swing) re-triggers while held, so model a tap.
        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = (down && !anyWave && !anyRobot) ? holdEstimate() : -1;

        SimEnd end;
        int base = simulate(layer, p1, p2, dual, 0, down, horizon, releaseAfter, &end);
        if (base >= horizon) {
            buf.appliedInput = down;
            return false;
        }

        int landed = 0;
        int saved = searchDelay(layer, p1, p2, dual, down, anyWave, horizon, maxDelay, releaseAfter, base, &landed);
        if (saved <= 0) {
            buf.appliedInput = down;
            return false;
        }

        say("SAVED +{} ticks ({:g} frames) {}  (now: {} -> delayed: {})  [limit {}]", saved, saved * TICK, down ? "PRESS" : "RELEASE",
            base, landed, maxDelay);
        buf.shift = saved;
        queueInput(saved, down);
        return true;
    }

    void cycleStage() {
        buf.stage = (buf.stage + 1) % STAGE_COUNT;
        resetPending();
        applyStage();
        readSettings();
        updateLabel();
        say("F6 -> {}", stageText());
    }

    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if (buf.sinceInput <= 998) {
            ++buf.sinceInput;
        }

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        if (player->m_isShip || (dual && layer->m_player2->m_isShip)) {
            resetPending();
            if (buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput);
            }
            return;
        }

        if (buf.queue.empty()) {
            if (buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput);
            }
            return;
        }

        // Check before decrementing: an input queued with N ticks fires right before
        // physics step N, which is exactly what simulate(delay = N) tested.
        while (!buf.queue.empty() && buf.queue.front().ticksLeft <= 0) {
            bool down = buf.queue.front().down;
            buf.queue.pop_front();
            if (down != buf.appliedInput) {
                sendButton(layer, down);
            }
        }
        for (auto& q : buf.queue) {
            --q.ticksLeft;
        }
        if (buf.queue.empty()) {
            buf.shift = 0;
        }
    }
}

$on_mod(Loaded) {
    buf.console = Mod::get()->getSettingValue<bool>("debug-console");
    readSettings();
    applyStage();
    if (buf.console) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Predictive Input Buffer - Live Console");
    }
    say("v2.0 loaded. F6 cycles: 0.5f -> 1f -> 2f -> 4f -> 6f -> 8f -> custom -> off");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && isGhost(player)) {
            buf.ghostDied = true;
            return;
        }
        resetPending();
        buf.appliedInput = false;
        buf.rapid = 0;
        PlayLayer::destroyPlayer(player, object);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        readSettings();
        applyStage();
        buf.ghost1 = makeGhost(this);
        buf.ghost2 = makeGhost(this);

        auto win = CCDirector::get()->getWinSize();
        buf.label = CCLabelBMFont::create(stageText().c_str(), "bigFont.fnt");
        buf.label->setScale(0.3f);
        buf.label->setOpacity(150);
        buf.label->setAnchorPoint({ 0.f, 1.f });
        buf.label->setPosition({ 5.f, win.height - 5.f });
        buf.label->setZOrder(1000);
        this->addChild(buf.label);
        updateLabel();

        buf.realInput = false;
        buf.appliedInput = false;
        buf.isPlayer1 = true;
        buf.sinceInput = 999;
        buf.rapid = 0;
        return true;
    }

    void onQuit() {
        dropGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        resetPending();
        PlayLayer::resetLevel();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        dropGhosts();
        return ok;
    }
};

class $modify(BufHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (buf.simulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (buf.simulating) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(BufGameObject, GameObject) {
    void playShineEffect() {
        if (buf.simulating) {
            return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
    }

    void update(float dt) {
        if (!buf.simulating) {
            auto* layer = GameManager::get()->m_playLayer;
            if (layer && this == layer->m_player1) {
                ++tickCount;
                static bool f6Was = false;
                bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
                if (f6 && !f6Was && gdFocused()) {
                    cycleStage();
                }
                f6Was = f6;
                if (buf.enabled) {
                    deliverPending(layer, this);
                }
            }
        }

        PlayerObject::update(dt);

        // The ghost must step with exactly the dt the real player gets.
        if (!buf.simulating && !isGhost(this) && dt > 0.f) {
            buf.tickLength = dt;
        }
    }

    void incrementJumps() {
        if (buf.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (buf.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(this)) {
                return;
            }
            snapshotObject(ring);
        }
        PlayerObject::ringJump(ring, skipCheck);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", HOOK_PRIORITY);
    }

    // During a simulation the ghost may use each object once; the real activation
    // state is snapshotted and restored afterwards so the real player is unaffected.
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player) || !object || buf.simUsed.contains(object)) {
                return false;
            }
            snapshotObject(object);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!buf.simulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < objectCount) {
            relevant.reserve(objectCount);
        }

        CCRect ghostRect = player->getObjectRect();
        for (int i = 0; i < objectCount; ++i) {
            auto* object = objects->at(i);
            if (!object) {
                continue;
            }
            if (hasType(COLLIDING_TYPES, object)) {
                relevant.push_back(object);
                continue;
            }
            if (!buf.smart) {
                continue;
            }
            if (hasType(INTERACT_TYPES, object)) {
                relevant.push_back(object);
                continue;
            }
            bool stop = hasType(STOP_TYPES, object);
            float speed = speedPortalValue(object->m_objectID);
            if ((stop || speed > 0.f) && ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    buf.ghostHitPortal = true;
                }
                else {
                    player->m_playerSpeed = speed;
                }
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) {
            recordRealInput(down);
        }
        if (!buf.simulating && button == 1 && !buf.injecting && intercept(this, down, isPlayer1)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player)) {
                return;
            }
            snapshotObject(ring);
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    // Gravity portals / blue pads / blue orbs: flip only the ghost, no layer effects.
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (buf.simulating) {
            if (buf.smart && isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    // Never let ghost activity fire event triggers in the real level.
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    // Safety net in case a mode portal slips through: end prediction, change nothing.
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (buf.simulating) {
            buf.ghostHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};
