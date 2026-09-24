// emerald_touch.cpp — policy entry points, gesture routing, macro runner,
// diagnostics rings. See emerald_touch.h.

#include "emerald_touch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "emerald_extended_view.h"
#include "host_overlay.h"
#include "host_window.h"
#include "mini_json.h"
#include "mod_runtime.h"

namespace emerald::touch {

using gbarecomp::Gesture;
using gbarecomp::GestureKind;
using gbarecomp::HostOverlay;
using gbarecomp::KeySynth;
using gbarecomp::OverlayColor;
using gbarecomp::OverlaySpace;
namespace k = gbarecomp;

Core& core() {
    static Core c;
    return c;
}

// ── Macro runner ─────────────────────────────────────────────────────────

void MacroRunner::start(const char* label, std::vector<Step> steps,
                        std::uint32_t scene_signature, bool abort_on_scene_change) {
    steps_.assign(steps.begin(), steps.end());
    label_ = label ? label : "macro";
    signature_ = scene_signature;
    abort_on_change_ = abort_on_scene_change;
    frames_ = 0;
}

void MacroRunner::cancel(const char* why) {
    if (steps_.empty()) return;
    std::fprintf(stderr, "[emerald:touch] macro '%s' cancelled: %s\n", label_.c_str(),
                 why ? why : "");
    record_action(core().frame, "macro", scene_name(core().scene.kind), "cancel", 0, 0,
                  frames_);
    steps_.clear();
}

void MacroRunner::frame(FrameCtx& ctx) {
    if (steps_.empty()) return;
    ++frames_;
    if (abort_on_change_ && ctx.scene.signature() != signature_ && ctx.synth.idle()) {
        // The screen the macro was built for is gone (the game moved on).
        steps_.clear();
        return;
    }
    if (frames_ > 900) {
        cancel("timeout");
        return;
    }
    switch (steps_.front()(ctx)) {
        case StepResult::Continue: break;
        case StepResult::Done:
            steps_.pop_front();
            // A completed step may legitimately change the scene (A pressed).
            signature_ = ctx.scene.signature();
            if (steps_.empty())
                record_action(core().frame, "macro", scene_name(ctx.scene.kind), "done",
                              0, 0, frames_);
            break;
        case StepResult::Fail:
            cancel("step failed");
            break;
    }
}

Step step_press(std::uint16_t keys, int press_frames, int release_frames) {
    auto queued = std::make_shared<bool>(false);
    return [=](FrameCtx& ctx) {
        if (!*queued) {
            ctx.synth.tap(keys, press_frames, release_frames);
            *queued = true;
            return StepResult::Continue;
        }
        return ctx.synth.idle() ? StepResult::Done : StepResult::Continue;
    };
}

Step step_wait(int frames) {
    auto left = std::make_shared<int>(frames);
    return [=](FrameCtx&) {
        return --*left <= 0 ? StepResult::Done : StepResult::Continue;
    };
}

Step step_until(std::function<bool(const FrameCtx&)> done, int timeout_frames) {
    auto left = std::make_shared<int>(timeout_frames);
    return [=](FrameCtx& ctx) {
        if (done(ctx)) return StepResult::Done;
        return --*left <= 0 ? StepResult::Fail : StepResult::Continue;
    };
}

Step step_navigate(std::function<int(const FrameCtx&)> read, int target,
                   std::function<std::uint16_t(int, int)> choose, int max_presses) {
    struct State { int presses = 0; int last = -9999; int stuck = 0; };
    auto st = std::make_shared<State>();
    return [=](FrameCtx& ctx) {
        if (!ctx.synth.idle()) return StepResult::Continue;
        const int cur = read(ctx);
        if (cur == target) return StepResult::Done;
        if (cur < -1000) return StepResult::Fail;
        // Input delays (script multichoice) swallow early presses: allow a few
        // retries without cursor movement before giving up.
        st->stuck = cur == st->last ? st->stuck + 1 : 0;
        st->last = cur;
        if (st->stuck > 6 || st->presses >= max_presses) return StepResult::Fail;
        const std::uint16_t key = choose(cur, target);
        if (!key) return StepResult::Fail;
        ctx.synth.tap(key, 1, 2);
        ++st->presses;
        return StepResult::Continue;
    };
}

// ── Coordinates ──────────────────────────────────────────────────────────

bool view_to_native(float vx, float vy, float* nx, float* ny) {
    const Core& c = core();
    const int x = static_cast<int>(std::floor(vx)) - static_cast<int>(c.extra_left);
    const int y = static_cast<int>(std::floor(vy)) - static_cast<int>(c.extra_top);
    int sx = x, sy = y;
    ui_view_source(x, y, &sx, &sy);
    *nx = static_cast<float>(sx) + (vx - std::floor(vx));
    *ny = static_cast<float>(sy) + (vy - std::floor(vy));
    return sx >= 0 && sy >= 0 && sx < 240 && sy < 160;
}

// ── Diagnostics rings ────────────────────────────────────────────────────

namespace {
struct ActionSample {
    std::uint64_t seq = 0, frame = 0;
    char gesture[20] = {}, scene[18] = {}, decision[28] = {};
    float nx = 0, ny = 0;
    int detail = 0;
};
struct SceneSample {
    std::uint64_t seq = 0, frame = 0;
    SceneKind kind = SceneKind::Unknown;
    std::uint32_t cb2 = 0;
    bool field_free = false, menu = false, list = false, text_wait = false, start = false;
    BattleControl control = BattleControl::None;
    int menu_window = 0xFF, list_task = 0xFF;
};
std::mutex g_ring_m;
constexpr std::size_t kActionRing = 2048, kSceneRing = 4096;
ActionSample g_actions[kActionRing];
SceneSample g_scenes[kSceneRing];
std::uint64_t g_action_seq = 0, g_scene_seq = 0;
std::uint32_t g_last_scene_sig = 0xFFFFFFFFu;
std::map<std::string, std::uint64_t> g_unclaimed;   // coverage: scene key -> count
bool g_mobile = false;
bool g_native_scenes = false;
int g_native_hold = 0;
bool g_native_requested = false;

struct Ripple { float vx, vy; std::uint32_t t; OverlayColor color; };
std::vector<Ripple> g_ripples;
std::uint32_t g_now = 0;

void copy_str(char* dst, std::size_t n, const char* src) {
    std::snprintf(dst, n, "%s", src ? src : "");
}

void record_scene(const SceneState& s) {
    const std::uint32_t sig = s.signature() ^ (s.field.free ? 0x40000000u : 0) ^
                              (s.text.waiting ? 0x20000000u : 0);
    if (sig == g_last_scene_sig) return;
    g_last_scene_sig = sig;
    std::lock_guard<std::mutex> lk(g_ring_m);
    SceneSample& e = g_scenes[g_scene_seq % kSceneRing];
    e.seq = ++g_scene_seq;
    e.frame = s.frame;
    e.kind = s.kind;
    e.cb2 = s.cb2;
    e.field_free = s.field.free;
    e.menu = s.menu.live;
    e.list = s.list.live;
    e.text_wait = s.text.waiting;
    e.start = s.start_menu;
    e.control = s.battle.control;
    e.menu_window = s.menu.live ? s.menu.window : 0xFF;
    e.list_task = s.list.live ? s.list.task : 0xFF;
}
}  // namespace

void record_action(std::uint64_t frame, const char* gesture, const char* scene,
                   const char* decision, float nx, float ny, int detail) {
    std::lock_guard<std::mutex> lk(g_ring_m);
    ActionSample& a = g_actions[g_action_seq % kActionRing];
    a.seq = ++g_action_seq;
    a.frame = frame;
    copy_str(a.gesture, sizeof(a.gesture), gesture);
    copy_str(a.scene, sizeof(a.scene), scene);
    copy_str(a.decision, sizeof(a.decision), decision);
    a.nx = nx;
    a.ny = ny;
    a.detail = detail;
}

void record_unclaimed(const SceneState& s, const char* gesture) {
    char key[160];
    std::snprintf(key, sizeof(key), "%s|cb2=%08X|ctl=%s|menu=%d|list=%d|text=%d|%s",
                  scene_name(s.kind), s.cb2, battle_control_name(s.battle.control),
                  s.menu.live ? 1 : 0, s.list.live ? 1 : 0,
                  (s.text.waiting || s.text.printing) ? 1 : 0, gesture);
    std::lock_guard<std::mutex> lk(g_ring_m);
    ++g_unclaimed[key];
}

void set_mobile(bool mobile) {
    g_mobile = mobile;
    const char* env = std::getenv("GBARECOMP_EMERALD_NATIVE_SCENES");
    g_native_scenes = env ? (env[0] && env[0] != '0') : mobile;
}

// ── Gesture routing ──────────────────────────────────────────────────────

namespace {

const char* gname(GestureKind k) { return k::gesture_kind_name(k); }

void ripple(const Gesture& g, OverlayColor color) {
    g_ripples.push_back({g.view_x, g.view_y, g_now, color});
    if (g_ripples.size() > 16) g_ripples.erase(g_ripples.begin());
}

void press_b(Core& c, const char* why, const Gesture& g, float nx, float ny) {
    c.macro.start(why, {step_press(k::kGbaKeyB)}, c.scene.signature(), false);
    record_action(c.frame, gname(g.kind), scene_name(c.scene.kind), why, nx, ny, 0);
    ripple(g, {83, 196, 255, 200});
    k::host_haptic_pulse(12, 0.4f);
}

void press_a(Core& c, const char* why, const Gesture& g, float nx, float ny) {
    c.macro.start(why, {step_press(k::kGbaKeyA)}, c.scene.signature(), false);
    record_action(c.frame, gname(g.kind), scene_name(c.scene.kind), why, nx, ny, 0);
    ripple(g, {255, 214, 74, 200});
}

bool text_active(const SceneState& s) {
    return s.text.waiting || s.text.printing || s.text.script_wait_button;
}

// Universal fallback for UIs without a precise provider: a drag steps the
// D-pad, one press per 12 native pixels along the dominant axis.
struct DpadDrag { bool active = false; float last_x = 0, last_y = 0, ax = 0, ay = 0; };
DpadDrag g_dpad;

bool dpad_drag(Core& c, const SceneState& s, const Gesture& g) {
    if (s.kind == SceneKind::Overworld || s.kind == SceneKind::Boot) return false;
    float nx = 0, ny = 0;
    view_to_native(g.view_x, g.view_y, &nx, &ny);
    if (g.kind == GestureKind::DragBegin) {
        float sx = 0, sy = 0;
        view_to_native(g.start_view_x, g.start_view_y, &sx, &sy);
        g_dpad = {true, sx, sy, 0, 0};
        record_unclaimed(s, "drag");
    }
    if (!g_dpad.active) return false;
    if (g.kind == GestureKind::DragEnd || g.kind == GestureKind::Cancel) {
        g_dpad.active = false;
        return true;
    }
    g_dpad.ax += nx - g_dpad.last_x;
    g_dpad.ay += ny - g_dpad.last_y;
    g_dpad.last_x = nx;
    g_dpad.last_y = ny;
    constexpr float kStep = 12.0f;
    while (std::fabs(g_dpad.ax) >= kStep || std::fabs(g_dpad.ay) >= kStep) {
        if (std::fabs(g_dpad.ax) >= std::fabs(g_dpad.ay)) {
            const bool right = g_dpad.ax > 0;
            c.synth.tap(right ? k::kGbaKeyRight : k::kGbaKeyLeft, 1, 1);
            g_dpad.ax += right ? -kStep : kStep;
        } else {
            const bool down = g_dpad.ay > 0;
            c.synth.tap(down ? k::kGbaKeyDown : k::kGbaKeyUp, 1, 1);
            g_dpad.ay += down ? -kStep : kStep;
        }
        record_action(c.frame, "drag", scene_name(s.kind), "fallback:dpad", nx, ny, 0);
    }
    return true;
}

void route(Core& c, const Gesture& g) {
    const SceneState& s = c.scene;
    float nx = 0, ny = 0;
    view_to_native(g.view_x, g.view_y, &nx, &ny);

    switch (g.kind) {
        case GestureKind::Tap: {
            if (c.macro.busy()) {
                record_action(c.frame, "tap", scene_name(s.kind), "ignored:busy", nx, ny, 0);
                return;
            }
            if (s.battle.active && battle_panel_tap(c, s, g.drawable_x, g.drawable_y)) {
                ripple(g, {255, 214, 74, 200});
                return;
            }
            if (menus_tap(c, s, nx, ny) || battle_tap(c, s, nx, ny)) {
                ripple(g, {255, 214, 74, 200});
                k::host_haptic_pulse(10, 0.35f);
                return;
            }
            if (s.menu.live || s.list.live) {
                press_b(c, "tap-outside-menu:B", g, nx, ny);
                return;
            }
            if (text_active(s)) {
                press_a(c, "text:A", g, nx, ny);
                return;
            }
            if (field_tap(c, s, g, nx, ny)) {
                ripple(g, {120, 235, 171, 200});
                return;
            }
            if (screens_tap(c, s, nx, ny)) {
                ripple(g, {255, 214, 74, 200});
                return;
            }
            if (s.kind != SceneKind::Overworld && s.kind != SceneKind::Boot &&
                s.kind != SceneKind::Battle) {
                // Universal fallback for screens without a precise provider:
                // tap confirms. Logged as unmapped so coverage stays honest.
                record_unclaimed(s, "tap");
                press_a(c, "fallback:A", g, nx, ny);
                return;
            }
            record_action(c.frame, "tap", scene_name(s.kind), "unclaimed", nx, ny, 0);
            record_unclaimed(s, "tap");
            return;
        }
        case GestureKind::LongPress: {
            if (s.field.free && !s.menu.live && !text_active(s) && !c.macro.busy()) {
                c.macro.start("long-press:Start", {step_press(k::kGbaKeyStart)},
                              s.signature(), false);
                record_action(c.frame, "long_press", scene_name(s.kind), "start-menu", nx,
                              ny, 0);
                ripple(g, {255, 255, 255, 220});
                k::host_haptic_pulse(25, 0.6f);
                return;
            }
            record_action(c.frame, "long_press", scene_name(s.kind), "unclaimed", nx, ny, 0);
            record_unclaimed(s, "long_press");
            return;
        }
        case GestureKind::TwoFingerTap:
            press_b(c, "two-finger:B", g, nx, ny);
            return;
        case GestureKind::Back:
            if (s.field.free && !s.menu.live && !s.list.live && !text_active(s)) {
                // Nothing to cancel on the free overworld: offer settings.
                k::host_request_settings_menu();
                record_action(c.frame, "back", scene_name(s.kind), "settings-menu", 0, 0, 0);
                return;
            }
            press_b(c, "back:B", g, nx, ny);
            return;
        case GestureKind::DragBegin:
        case GestureKind::DragMove:
        case GestureKind::DragEnd:
        case GestureKind::Cancel:
            if (menus_drag(c, s, g)) return;
            if (field_drag(c, s, g)) return;
            if (dpad_drag(c, s, g)) return;
            if (g.kind == GestureKind::DragEnd) {
                record_action(c.frame, "drag", scene_name(s.kind), "unclaimed", nx, ny, 0);
                record_unclaimed(s, "drag");
            }
            return;
        case GestureKind::Swipe: {
            // Horizontal swipes page through multi-page screens.
            const bool horizontal = g.swipe == k::SwipeDirection::Left ||
                                    g.swipe == k::SwipeDirection::Right;
            const bool paged = s.kind == SceneKind::Bag || s.kind == SceneKind::Summary ||
                               s.kind == SceneKind::Storage || s.kind == SceneKind::Pokedex;
            if (horizontal && paged && !c.macro.busy()) {
                // Swipe left reveals the next page (content moves left).
                const std::uint16_t key = g.swipe == k::SwipeDirection::Left
                    ? k::kGbaKeyRight : k::kGbaKeyLeft;
                c.macro.start("swipe:page", {step_press(key)}, s.signature(), false);
                record_action(c.frame, "swipe", scene_name(s.kind), "page", nx, ny,
                              g.swipe == k::SwipeDirection::Left ? 1 : -1);
                return;
            }
            return;  // swipes elsewhere are just the tail of a drag
        }
        case GestureKind::ThreeFingerTap:
            return;  // engine-owned (settings)
    }
}

}  // namespace

std::uint16_t input_frame(const k::TouchFrameInfo* f) {
    Core& c = core();
    c.info = f;
    c.frame = f->frame_count;
    c.extra_left = f->extra_left;
    c.extra_top = f->extra_top;
    c.view_w = f->view_width;
    c.view_h = f->view_height;
    g_now = f->host_ms;
    const guest::Mem mem = guest::Mem::current();
    c.scene = classify(mem, f->frame_count);
    record_scene(c.scene);

    // Native presentation for margin-less scenes (mobile): hysteresis so a
    // momentary callback change never bounces the geometry.
    const bool wants_native = g_native_scenes && c.scene.kind != SceneKind::Overworld &&
                              c.scene.kind != SceneKind::Unknown;
    g_native_hold = wants_native ? std::min(g_native_hold + 1, 60) : 0;
    g_native_requested = g_native_hold >= 8;

    for (std::size_t i = 0; i < f->gesture_count; ++i) route(c, f->gestures[i]);

    // Hold A while a stationary finger rests during text printing.
    bool hold = false;
    if (c.scene.text.printing && !c.macro.busy()) {
        for (std::size_t i = 0; i < f->point_count; ++i) {
            const auto& p = f->points[i];
            if (!p.moved && f->host_ms - p.down_ms > 220) hold = true;
        }
    }
    if (hold != c.holding_text) {
        if (hold) c.synth.hold(k::kGbaKeyA); else c.synth.release(k::kGbaKeyA);
        c.holding_text = hold;
    }

    FrameCtx ctx{mem, c.scene, c.synth};
    field_frame(c, ctx);
    c.macro.frame(ctx);
    const std::uint16_t keys = c.synth.frame();
    c.info = nullptr;
    return keys;
}

void presentation_request(k::RunOptions::PresentationRequest* r) {
    r->native_view = g_native_requested;
    r->anchor_top = g_native_requested;
}

// ── Overlay ──────────────────────────────────────────────────────────────

void host_overlay(HostOverlay* ov) {
    if (!ov) return;
    const std::uint32_t now = ov->host_ms();
    g_ripples.erase(std::remove_if(g_ripples.begin(), g_ripples.end(),
                                   [now](const Ripple& r) { return now - r.t > 320; }),
                    g_ripples.end());
    for (const Ripple& r : g_ripples) {
        const float t = static_cast<float>(now - r.t) / 320.0f;
        OverlayColor c = r.color;
        c.a = static_cast<std::uint8_t>(c.a * (1.0f - t));
        ov->stroke_circle(r.vx, r.vy, 5.0f + 10.0f * t, 1.5f, c, OverlaySpace::View);
    }
    field_overlay(*ov);
    if (core().scene.battle.active) battle_overlay(*ov, core().scene);

    // Long-press cue on the free overworld (the gesture opens Start).
    const SceneState& s = core().scene;
    if (s.field.free && !s.menu.live) {
        for (const auto& p : k::TouchHub::instance().points()) {
            if (p.moved || p.long_pressed) continue;
            const std::uint32_t held = now - p.down_ms;
            if (held < 120) continue;
            const float progress = std::min(1.0f, held / 450.0f);
            ov->arc(p.drawable_x, p.drawable_y, 8.0f * ov->drawable_px_per_mm(),
                    1.2f * ov->drawable_px_per_mm(), 0.0f, progress,
                    {255, 255, 255, 210}, OverlaySpace::Drawable);
        }
    }
}

// ── TCP ──────────────────────────────────────────────────────────────────

int tcp_command(const char* request, void (*write)(void*, const char*, std::size_t),
                void* ctx) {
    if (!request || !std::strstr(request, "\"emerald_touch")) return 0;
    gbarecomp::json::Value req;
    if (!gbarecomp::json::parse(request, req) || !req.is_object()) return 0;
    const std::string cmd = req.str("cmd");
    std::string out;
    auto emit = [&] { write(ctx, out.data(), out.size()); };
    const std::uint64_t since = static_cast<std::uint64_t>(req.integer("since", 0));
    const std::size_t limit = static_cast<std::size_t>(req.integer("limit", 256));
    char buf[512];
    if (cmd == "emerald_touch_status") {
        const SceneState& s = core().scene;
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"frame\":%llu,\"scene\":\"%s\",\"cb2\":\"0x%08X\","
            "\"field_free\":%s,\"menu\":%s,\"menu_window\":%d,\"menu_cursor\":%d,"
            "\"list\":%s,\"list_task\":%d,\"list_selected\":%d,\"list_scroll\":%d,"
            "\"text_waiting\":%s,\"text_printing\":%s,\"start_menu\":%s,"
            "\"battle_control\":\"%s\",\"macro\":\"%s\",\"native_view\":%s,\"mobile\":%s}",
            static_cast<unsigned long long>(s.frame), scene_name(s.kind), s.cb2,
            s.field.free ? "true" : "false", s.menu.live ? "true" : "false",
            s.menu.window, s.menu.cursor, s.list.live ? "true" : "false", s.list.task,
            s.list.selected, s.list.scroll, s.text.waiting ? "true" : "false",
            s.text.printing ? "true" : "false", s.start_menu ? "true" : "false",
            battle_control_name(s.battle.control),
            core().macro.busy() ? core().macro.label().c_str() : "",
            g_native_requested ? "true" : "false", g_mobile ? "true" : "false");
        out = buf;
        out.pop_back();  // reopen the object for hit geometry
        // Native-space hit rectangles of the live menu / list (tests, debugging).
        out += ",\"view_offset\":[" + std::to_string(core().extra_left) + "," +
               std::to_string(core().extra_top) + "],\"menu_items\":[";
        if (s.menu.live) {
            for (int i = s.menu.min; i <= s.menu.max; ++i) {
                const Rect r = s.menu.item(i);
                std::snprintf(buf, sizeof(buf), "%s[%d,%d,%d,%d]", i == s.menu.min ? "" : ",",
                              r.x, r.y, r.w, r.h);
                out += buf;
            }
        }
        out += "],\"list_rows\":[";
        if (s.list.live) {
            const int visible = std::min(s.list.max_showed, s.list.total - s.list.scroll);
            for (int r = 0; r < visible; ++r) {
                const Rect rr = s.list.row(r);
                std::snprintf(buf, sizeof(buf), "%s[%d,%d,%d,%d]", r ? "," : "", rr.x, rr.y,
                              rr.w, rr.h);
                out += buf;
            }
        }
        out += "]}";
        emit();
        return 1;
    }
    std::lock_guard<std::mutex> lk(g_ring_m);
    if (cmd == "emerald_touch_actions") {
        out = "{\"ok\":true,\"actions\":[";
        std::size_t n = 0;
        const std::uint64_t first = g_action_seq > kActionRing ? g_action_seq - kActionRing : 0;
        for (std::uint64_t i = std::max(first, since); i < g_action_seq && n < limit; ++i, ++n) {
            const ActionSample& a = g_actions[i % kActionRing];
            std::snprintf(buf, sizeof(buf),
                "%s{\"seq\":%llu,\"frame\":%llu,\"gesture\":\"%s\",\"scene\":\"%s\","
                "\"decision\":\"%s\",\"nx\":%.1f,\"ny\":%.1f,\"detail\":%d}",
                n ? "," : "", static_cast<unsigned long long>(a.seq),
                static_cast<unsigned long long>(a.frame), a.gesture, a.scene, a.decision,
                a.nx, a.ny, a.detail);
            out += buf;
        }
        out += "]}";
        emit();
        return 1;
    }
    if (cmd == "emerald_touch_scenes") {
        out = "{\"ok\":true,\"scenes\":[";
        std::size_t n = 0;
        const std::uint64_t first = g_scene_seq > kSceneRing ? g_scene_seq - kSceneRing : 0;
        for (std::uint64_t i = std::max(first, since); i < g_scene_seq && n < limit; ++i, ++n) {
            const SceneSample& e = g_scenes[i % kSceneRing];
            std::snprintf(buf, sizeof(buf),
                "%s{\"seq\":%llu,\"frame\":%llu,\"scene\":\"%s\",\"cb2\":\"0x%08X\","
                "\"field_free\":%s,\"menu\":%s,\"menu_window\":%d,\"list\":%s,"
                "\"list_task\":%d,\"text_wait\":%s,\"start_menu\":%s,\"control\":\"%s\"}",
                n ? "," : "", static_cast<unsigned long long>(e.seq),
                static_cast<unsigned long long>(e.frame), scene_name(e.kind), e.cb2,
                e.field_free ? "true" : "false", e.menu ? "true" : "false", e.menu_window,
                e.list ? "true" : "false", e.list_task, e.text_wait ? "true" : "false",
                e.start ? "true" : "false", battle_control_name(e.control));
            out += buf;
        }
        out += "]}";
        emit();
        return 1;
    }
    if (cmd == "emerald_touch_coverage") {
        out = "{\"ok\":true,\"unclaimed\":{";
        bool first = true;
        for (const auto& [key, count] : g_unclaimed) {
            out += first ? "" : ",";
            first = false;
            gbarecomp::json::append_quoted(out, key);
            out += ":" + std::to_string(count);
        }
        out += "}}";
        emit();
        return 1;
    }
    if (cmd == "emerald_touch_field") {
        out = field_json();
        emit();
        return 1;
    }
    out = gbarecomp::json::error_reply("unknown emerald_touch command");
    emit();
    return 1;
}

}  // namespace emerald::touch

// Probes are statically registered; mod activation disables every entry hook
// before its reset callbacks run, so re-enable them from a reset callback.
GBA_MOD_CONSTRUCTOR(emerald_register_touch_probes) {
    emerald::touch::register_scene_probes();
    gba_mod_register_reset_callback(emerald::touch::enable_scene_probes);
}
