// emerald_touch.h — Pokémon Emerald touch-first input policy.
//
// Public entry points are wired into RunOptions by main.cpp. Internally the
// policy classifies the scene each frame (emerald_scene), routes gestures to
// scene providers, and turns their decisions into verified key macros driven
// through the engine's KeySynth. Guest state is only read; the game is
// steered exclusively by synthesized presses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "emerald_scene.h"
#include "input_synth.h"
#include "runtime.h"
#include "touch_input.h"

namespace gbarecomp { class HostOverlay; }

namespace emerald::touch {

// RunOptions wiring.
std::uint16_t input_frame(const gbarecomp::TouchFrameInfo* frame);
void host_overlay(gbarecomp::HostOverlay* overlay);
void presentation_request(gbarecomp::RunOptions::PresentationRequest* request);
int tcp_command(const char* request, void (*write)(void*, const char*, std::size_t),
                void* ctx);
void set_mobile(bool mobile);   // mobile presentation policy (native 3:2 scenes)

constexpr std::uint32_t kGestureClaims =
    gbarecomp::kTouchClaimTap | gbarecomp::kTouchClaimLongPress |
    gbarecomp::kTouchClaimDrag | gbarecomp::kTouchClaimSwipe |
    gbarecomp::kTouchClaimTwoFingerTap | gbarecomp::kTouchClaimBack;

// ── Internal (shared by the touch translation units) ─────────────────────

struct FrameCtx {
    const guest::Mem& mem;
    const SceneState& scene;
    gbarecomp::KeySynth& synth;
};

// A verified, frame-stepped key macro. Each step runs once per frame until it
// reports Done (advance) or Fail (abort the macro).
enum class StepResult { Continue, Done, Fail };
using Step = std::function<StepResult(FrameCtx&)>;

class MacroRunner {
public:
    void start(const char* label, std::vector<Step> steps, std::uint32_t scene_signature,
               bool abort_on_scene_change = true);
    void cancel(const char* why);
    bool busy() const { return !steps_.empty(); }
    const std::string& label() const { return label_; }
    // Returns false when the macro ended this frame (done/failed/aborted).
    void frame(FrameCtx& ctx);
    int frames_running() const { return frames_; }

private:
    std::deque<Step> steps_;
    std::string label_;
    std::uint32_t signature_ = 0;
    bool abort_on_change_ = true;
    int frames_ = 0;
};

// Step builders.
Step step_press(std::uint16_t keys, int press_frames = 2, int release_frames = 2);
Step step_wait(int frames);
// Drive a cursor to `target` with directional taps, verifying the cursor the
// game reports after every press. `choose` returns the key moving `current`
// toward `target` (0 = no move possible).
Step step_navigate(std::function<int(const FrameCtx&)> read, int target,
                   std::function<std::uint16_t(int current, int target)> choose,
                   int max_presses = 32);
Step step_until(std::function<bool(const FrameCtx&)> done, int timeout_frames);

// Map a gesture point (logical view pixels) into native 240x160 screen space,
// undoing extended margins and anchored-window displacement.
bool view_to_native(float vx, float vy, float* nx, float* ny);

// Always-on rings + coverage (touch diagnostics).
void record_action(std::uint64_t frame, const char* gesture, const char* scene,
                   const char* decision, float nx, float ny, int detail);
void record_unclaimed(const SceneState& scene, const char* gesture);

// Scene providers: return true when the gesture was consumed.
struct Core;
bool menus_tap(Core& core, const SceneState& s, float nx, float ny);
bool menus_drag(Core& core, const SceneState& s, const gbarecomp::Gesture& g);
bool screens_tap(Core& core, const SceneState& s, float nx, float ny);
bool battle_tap(Core& core, const SceneState& s, float nx, float ny);
bool field_tap(Core& core, const SceneState& s, const gbarecomp::Gesture& g,
               float nx, float ny);
bool field_drag(Core& core, const SceneState& s, const gbarecomp::Gesture& g);
void field_frame(Core& core, FrameCtx& ctx);
void field_overlay(gbarecomp::HostOverlay& ov);
void battle_overlay(gbarecomp::HostOverlay& ov, const SceneState& s);
bool battle_panel_tap(Core& core, const SceneState& s, float dx, float dy);
std::string field_json();

struct Core {
    gbarecomp::KeySynth synth;
    MacroRunner macro;
    SceneState scene;
    std::uint64_t frame = 0;
    std::uint32_t extra_left = 0, extra_top = 0;
    std::uint32_t view_w = 240, view_h = 160;
    const gbarecomp::TouchFrameInfo* info = nullptr;  // valid during input_frame
    // Held text speed-up (a stationary finger during printing).
    bool holding_text = false;
};
Core& core();

}  // namespace emerald::touch
