// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks for Vulkan NVIDIA Reflex integration.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "gpu/bbgpu.h"
#include "gpu/shim/bbport_settings.h"
#include "gpu/shadps4/video_core/renderer_vulkan/vk_reflex.h"

#include "gpu/shim/common/logging/log.h"

// Local test stubs for Common::Log when running outside libbbgpu
namespace Common::Log {
bool ShouldLog(Level /*level*/) {
    return false;
}
void Write(std::string_view /*log_class*/, Level /*level*/, const char* /*file*/, int /*line*/,
           const char* /*function*/, std::string&& /*message*/) {}
} // namespace Common::Log



// Vulkan dynamic dispatcher storage definition for standalone test binary
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

// Stubs for bbgpu C exports when linking outside libbbgpu.a
extern "C" {
void bbgpu_reflex_pre_input_sample(void) {
    Vulkan::Reflex::PreInputSample();
}
void bbgpu_reflex_on_input_sampled(void) {
    Vulkan::Reflex::OnInputSampled();
}
void bbgpu_reflex_on_simulation_end(void) {
    Vulkan::Reflex::OnSimulationEnd();
}
uint64_t bbgpu_reflex_current_sim_frame_id(void) {
    return Vulkan::Reflex::CurrentSimFrameId();
}
}

int main() {
    using namespace Vulkan::Reflex;

    std::puts("Running test_reflex_cpu...");

    // 1. Initial / unsupported state verification
    assert(!IsSupported());
    assert(!IsActive());
    assert(CurrentSimFrameId() == 0);

    // 2. Pre-input hook pacing and once-per-frame gating verification
    // Calling PreInputSample repeatedly within the SAME simulation cycle must NOT increment frame ID
    PreInputSample();
    const uint64_t fid1 = CurrentSimFrameId();
    assert(fid1 == 1);

    // Explicitly call PreInputSample multiple times within the same frame: must stay on frame 1
    PreInputSample();
    assert(CurrentSimFrameId() == 1);

    PreInputSample();
    assert(CurrentSimFrameId() == 1);

    // Input sampled within the cycle
    OnInputSampled();
    assert(CurrentSimFrameId() == 1);

    // Ending the simulation cycle (flip consumed) allows the next frame to open. The close
    // carries the identity captured at flip acceptance.
    OnSimulationEnd(fid1);
    assert(CurrentSimFrameId() == 1);

    // Now next pre-input sample advances to frame 2 (exactly 1 increment)
    PreInputSample();
    const uint64_t fid2 = CurrentSimFrameId();
    assert(fid2 == 2);

    // Multiple calls within frame 2 stay on frame 2
    PreInputSample();
    assert(CurrentSimFrameId() == 2);

    OnInputSampled();
    OnSimulationEnd(fid2);

    // 2b. Owned-cycle closure. A close is validated against the cycle that is actually open,
    // under the cycle gate, before it emits SIMULATION_END or clears the open flag. A stale or
    // duplicate close must therefore never close a newer cycle the guest has opened since.
    {
        // Cycle A opens, is sampled, and is closed by its own captured id.
        PreInputSample();
        const uint64_t cycle_a = CurrentSimFrameId();
        assert(cycle_a == fid2 + 1);
        OnInputSampled();
        OnSimulationEnd(cycle_a);
        assert(CurrentSimFrameId() == cycle_a);

        // A duplicate close of the cycle that just ended is a no-op and must not corrupt the
        // state: the next pad read still opens exactly one new cycle.
        OnSimulationEnd(cycle_a);

        // Cycle B opens. A stale close for cycle A must not close or mark cycle B.
        PreInputSample();
        const uint64_t cycle_b = CurrentSimFrameId();
        assert(cycle_b == cycle_a + 1);
        OnSimulationEnd(cycle_a);
        // Still open: a pad read inside cycle B must not advance the id.
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b);

        // A close that matches neither the open id nor any closed id is also a no-op.
        OnSimulationEnd(cycle_b + 7);
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b);

        // A zero id can never close an open cycle.
        OnSimulationEnd(0);
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b);

        // The owning close ends cycle B; only then does the next pad read advance.
        OnSimulationEnd(cycle_b);
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b + 1);
        OnSimulationEnd(CurrentSimFrameId());
    }

    // 2c. A thread spamming stale/foreign closes must not close the active cycle, must not
    // corrupt its markers, and must not deadlock against the owning close.
    {
        const uint64_t stale_start = CurrentSimFrameId();
        std::atomic<bool> stop{false};
        std::thread stale_closer([&] {
            // Ids that never become the open cycle: a fixed sentinel plus an old id.
            while (!stop.load(std::memory_order_relaxed)) {
                OnSimulationEnd(0x5EED);
                OnSimulationEnd(stale_start);
            }
        });
        constexpr uint64_t kWantedCycles = 300;
        for (uint64_t i = 0; i < kWantedCycles; ++i) {
            PreInputSample();
            const uint64_t fid = CurrentSimFrameId();
            OnInputSampled();
            OnSimulationEnd(fid);
        }
        stop.store(true, std::memory_order_relaxed);
        stale_closer.join();
        assert(CurrentSimFrameId() == stale_start + kWantedCycles);
    }

    // 3. Thread attribution & context tracking
    SetRecordingFrameId(fid2);
    assert(RecordingFrameId() == fid2);
    SetRecordingFrameId(0);
    assert(RecordingFrameId() == 0);

    // 4. Systematic Lock Hierarchy & Deadlock-Free Concurrency Test
    // Hierarchy: SleepMutex -> WsiMutex
    // Test: Thread A holds SleepMutex and operates, Thread B acquires SleepMutex then WsiMutex (like Swapchain::Recreate)
    // while Thread C performs PreInputSample. Neither thread should deadlock.
    {
        std::atomic<bool> thread_started{false};
        std::atomic<bool> test_done{false};

        std::thread recreate_thread([&] {
            thread_started = true;
            for (int i = 0; i < 50; ++i) {
                std::lock_guard<std::mutex> sleep_lk{SleepMutex()};
                std::lock_guard<std::mutex> wsi_lk{WsiMutex()};
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            test_done = true;
        });

        while (!thread_started) {
            std::this_thread::yield();
        }

        while (!test_done) {
            PreInputSample();
            OnInputSampled();
            OnSimulationEnd();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        recreate_thread.join();
    }

    // 5. C API hooks (bbgpu_reflex_*) verification
    bbgpu_reflex_pre_input_sample();
    const uint64_t c_fid_curr = bbgpu_reflex_current_sim_frame_id();
    assert(c_fid_curr > 2);

    // Re-entrant calls stay at current frame
    bbgpu_reflex_pre_input_sample();
    assert(bbgpu_reflex_current_sim_frame_id() == c_fid_curr);

    bbgpu_reflex_on_input_sampled();
    bbgpu_reflex_on_simulation_end();

    // 6. Submission attribution sanitization without GPU/driver
    vk::SubmitInfo submit{};
    VkLatencySubmissionPresentIdNV attribution{};
    const uint64_t prep_id = PrepareSubmit(nullptr, c_fid_curr, submit, attribution);
    // Unsupported driver path returns 0 and does not corrupt submission
    assert(prep_id == 0);

    // 6b. Recording identity: a submission captured at frame 41 must be applied
    // immediately and must survive every later allocation of that submission, even while the
    // live counter the guest has advanced to (42, 43) changes underneath it. A new submission
    // captured at 44 replaces it. A batch that never captured an identity keeps the live value.
    {
        RecordingIdentity id;

        // Nothing captured yet: allocation follows the live counter.
        assert(id.Allocate(40) == 40);
        assert(id.RecordingId() == 40);

        // Submission accepted at 41. It must apply immediately, before any allocation.
        id.BeginSubmission(41);
        assert(id.RecordingId() == 41);
        assert(id.HasSubmission() && id.SubmissionId() == 41);

        // Several command buffers are allocated for the same submission while the live
        // counter advances. Every one of them must stay on 41.
        assert(id.Allocate(42) == 41);
        assert(id.Allocate(43) == 41);
        assert(id.RecordingId() == 41);

        // Next source boundary: a submission captured at 44 applies immediately and persists.
        id.BeginSubmission(44);
        assert(id.RecordingId() == 44);
        assert(id.Allocate(45) == 44);
        assert(id.Allocate(46) == 44);

        // An explicit owner (presenter) overrides and drops the submission capture.
        id.SetFrameId(90);
        assert(id.RecordingId() == 90);
        assert(!id.HasSubmission());
        assert(id.Allocate(91) == 91);

        // A captured frame 0 is legitimate and must not fall back to a later live value.
        id.BeginSubmission(0);
        assert(id.Allocate(92) == 0);
        assert(id.Allocate(93) == 0);
    }

    // The pure resolver keeps its documented meaning.
    assert(ResolveRecordingFrameId(42, 7, true) == 7);
    assert(ResolveRecordingFrameId(42, 0, true) == 0);
    assert(ResolveRecordingFrameId(42, 7, false) == 42);

    // 6c. Bounded guest backlog. The guest may have at most `cap` accepted source frames
    // outstanding; the presenter retires each one when its present call returns.
    {
        Backlog backlog;
        backlog.Configure(true, 2);
        assert(backlog.Enabled() && backlog.Cap() == 2);

        // Cap clamp and disable.
        backlog.Configure(true, 99);
        assert(backlog.Cap() == Backlog::MaxCap);
        backlog.Configure(false, 2);
        assert(!backlog.Enabled());
        assert(backlog.Gate(1)); // disabled gate never blocks
        backlog.Configure(true, 2);

        // A direct flip may present before SubmitFlip returns: retire first, then register.
        // The registration must be a no-op so the stale id cannot occupy a slot.
        backlog.Retire(50);
        backlog.Register(50);
        assert(backlog.Outstanding() == 0);

        // Two in flight fill the cap; the gate blocks until one is retired.
        backlog.Register(51);
        backlog.Register(52);
        assert(backlog.Outstanding() == 2);
        assert(!backlog.Gate(5)); // times out, drops the stale entries
        assert(backlog.Outstanding() == 0);
        assert(backlog.Timeouts() == 1);

        backlog.Register(53);
        backlog.Register(54);
        backlog.Retire(53);
        assert(backlog.Outstanding() == 1);
        assert(backlog.Gate(5)); // room again

        // Duplicate registration of a frame already in flight must not consume another slot.
        // FIFO order: list is [54]; register 55 twice -> [54, 55].
        backlog.Register(55);
        backlog.Register(55);
        assert(backlog.Outstanding() == 2);
        backlog.Retire(55);
        assert(backlog.Outstanding() == 1);

        // FIFO retirement: retiring the older 54 leaves the newer 55 in place; a new 56 can
        // then register and be retired, and the list must be empty.
        backlog.Retire(54);
        assert(backlog.Outstanding() == 0);
        backlog.Register(56);
        backlog.Retire(56);
        assert(backlog.Outstanding() == 0);
        backlog.Retire(56); // already retired: harmless
        assert(backlog.Outstanding() == 0);

        // Sparse ids: only registered ones count.
        backlog.Register(61);
        backlog.Register(65);
        assert(backlog.Outstanding() == 2);
        backlog.Retire(65);
        assert(backlog.Outstanding() == 1);
        backlog.Retire(61);
        assert(backlog.Outstanding() == 0);

        // A queued EOP flip that fails is cancelled by its exact id.
        backlog.Register(66);
        backlog.Register(67);
        backlog.Cancel(66);
        assert(backlog.Outstanding() == 1);
        backlog.Cancel(66); // idempotent
        assert(backlog.Outstanding() == 1);
        backlog.Retire(67);
        assert(backlog.Outstanding() == 0);

        // Disabling clears the backlog and wakes a blocked gate.
        backlog.Register(68);
        backlog.Register(69);
        assert(backlog.Outstanding() == 2);
        std::thread unblock([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            backlog.Configure(false, 2);
        });
        assert(backlog.Gate(500)); // returns true once disabled
        unblock.join();
        assert(!backlog.Enabled() && backlog.Outstanding() == 0);
        // A mode transition must not forget a presentation which preceded registration.
        backlog.Retire(70);
        backlog.Configure(true, 2);
        backlog.Register(70);
        assert(backlog.Outstanding() == 0);
    }

    // 6d. A retire that arrives while the gate is blocked wakes it (two in flight, retire one).
    {
        Backlog backlog;
        backlog.Configure(true, 2);
        backlog.Register(90);
        backlog.Register(91);
        std::thread retire([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            backlog.Retire(90);
        });
        assert(backlog.Gate(500));
        retire.join();
        assert(backlog.Outstanding() == 1);
        backlog.Retire(91);
        assert(backlog.Outstanding() == 0);
    }

    // 6e. Accepted-flip tickets. The bounded backlog registers/retires by a monotonic ticket,
    // kept separate from the guest simulation content id (which can repeat across accepted
    // flips). Tickets are allocated before enqueue and the allocate/enqueue orders can differ
    // across guest threads, so retirement is matched exactly rather than by a high-water mark.
    {
        // Strictly increasing and therefore unique; never reset by a mode change.
        uint64_t last_ticket = AllocateFlipTicket();
        assert(last_ticket > 0);
        for (int i = 0; i < 300; ++i) {
            const uint64_t next_ticket = AllocateFlipTicket();
            assert(next_ticket > last_ticket);
            last_ticket = next_ticket;
        }

        // Two accepted flips that share one simulation cycle still get distinct tickets and
        // must both occupy backlog slots (no content-id-style dedupe).
        const uint64_t cycle_ticket_a = AllocateFlipTicket();
        const uint64_t cycle_ticket_b = AllocateFlipTicket();
        assert(cycle_ticket_b > cycle_ticket_a);

        const uint64_t older_ticket = AllocateFlipTicket();
        const uint64_t newer_ticket = AllocateFlipTicket();
        assert(newer_ticket > older_ticket);

        {
            Backlog backlog;
            backlog.Configure(true, 4);

            backlog.Register(cycle_ticket_a);
            backlog.Register(cycle_ticket_b);
            assert(backlog.Outstanding() == 2);

            // A newer ticket retiring first must not suppress an older ticket whose Register has
            // not arrived yet, and the already-retired newer ticket is suppressed exactly.
            backlog.Retire(newer_ticket);
            backlog.Register(older_ticket);
            assert(backlog.Outstanding() == 3);
            backlog.Register(newer_ticket);
            assert(backlog.Outstanding() == 3);

            // Exact retirement is idempotent.
            backlog.Retire(newer_ticket);
            assert(backlog.Outstanding() == 3);

            backlog.Retire(cycle_ticket_a);
            backlog.Retire(cycle_ticket_b);
            backlog.Retire(older_ticket);
            assert(backlog.Outstanding() == 0);
        }
        {
            // Retained across Configure: a retirement recorded before the mode change is not
            // forgotten, so a late Register of the same exact ticket is still suppressed.
            Backlog backlog;
            backlog.Configure(true, 2);
            backlog.Retire(newer_ticket);
            backlog.Configure(true, 2);
            backlog.Register(newer_ticket);
            assert(backlog.Outstanding() == 0);
        }
    }

    // 7. Stubs for present and render markers do not crash when unsupported
    OnRenderSubmitBegin(c_fid_curr);
    OnRenderSubmitEnd(c_fid_curr);
    OnForeignSubmitBegin(c_fid_curr);
    OnForeignSubmitEnd(c_fid_curr);
    OnPresentBegin(c_fid_curr, true);
    OnPresentEnd(c_fid_curr, true);
    OnPresentBegin(c_fid_curr, false);
    OnPresentEnd(c_fid_curr, false);

    // 8. Timings query stub
    LatencyReport report{};
    assert(!QueryLatencyTimings(&report));

    // 9. Swapchain latency create info setup stub
    vk::SwapchainCreateInfoKHR sc_ci{};
    VkSwapchainLatencyCreateInfoNV lat_ci{};
    PrepareSwapchainCreateInfo(sc_ci, lat_ci);

    // 10. Multi-threaded pad sample simulation
    // Verify that multiple threads polling pad simultaneously never leak duplicate frame IDs
    const uint64_t sim_before = CurrentSimFrameId();
    PreInputSample();
    const uint64_t sim_active = CurrentSimFrameId();
    assert(sim_active == sim_before + 1);

    for (int t = 0; t < 4; ++t) {
        std::thread([&] {
            bbgpu_reflex_pre_input_sample();
            bbgpu_reflex_on_input_sampled();
        }).join();
    }
    // Still sim_active because OnSimulationEnd has not been called!
    assert(CurrentSimFrameId() == sim_active);
    OnSimulationEnd();

    // Next PreInputSample moves to sim_active + 1
    PreInputSample();
    assert(CurrentSimFrameId() == sim_active + 1);
    OnSimulationEnd();

    // 11. Shutdown cleans up gracefully
    Shutdown();
    assert(!IsSupported());
    assert(!IsActive());

    std::puts("Vulkan NVIDIA Reflex CPU integration tests: PASS");
    return 0;
}
