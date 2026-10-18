"""Exercise production worker ownership with real C++ and fake stock services.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WorkerTests(unittest.TestCase):
    def test_flow_opt_in_wide_credits_replay_small_routes_and_lost_grants(self):
        self.check_case("flow-input")

    def test_flow_synthetic_playback_both_bridge_roles_pause_and_reclaim(self):
        self.check_case("flow-consumer")

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        cls.exe = directory / "worker-test"
        # Compile the actual upstream cleanup/lease functions with stock-service
        # mocks. Extraction avoids copying a second implementation into tests.
        functions = []
        for filename, names in (("patches/zlib_glue.c", ("cfw_cleanup_session", "present_shadow", "display_copy_hook")),
                                ("patches/image_buffers.c", ("cfw_shadow_buffer",)),
                                ("patches/settings_ext.c", ("cfw_fb_lease_active", "faceclaw_apply_control"))):
            source = (ROOT / filename).read_text(encoding="utf-8")
            for name in names:
                import re
                match = re.search(r"^[^\n]*\b" + name + r"\([^;\n]*\)\s*\{", source, re.M)
                if match is None:
                    raise AssertionError("missing production function: " + name)
                opening = source.index("{", match.start())
                depth, end = 1, opening + 1
                while depth:
                    depth += (source[end] == "{") - (source[end] == "}")
                    end += 1
                functions.append(source[match.start():end])
        (directory / "upstream_cleanup.h").write_text("\n\n".join(functions) + "\n", encoding="utf-8")
        common = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                  "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + str(directory)]
        objects = []
        for name, source, compiler, flags in (
            ("controller", "tests/h264/worker_test.c", "clang", ["-std=c11"]),
            ("runtime", "patches/h264/runtime.c", "clang", ["-std=c11"]),
            ("decoder", "patches/h264/g2_h264.cpp", "clang++",
             ["-std=c++23", "-fno-exceptions", "-fno-rtti",
              "-I" + str(ROOT / "third-party/sub0h264/components/sub0h264/include"),
              "-I" + str(ROOT / "third-party/sub0h264/components/sub0h264/src")]),
            ("stream", "tests/h264/worker_stream.cpp", "clang++", ["-std=c++23"]),
        ):
            obj = directory / (name + ".o")
            subprocess.run([compiler, *common, *flags, "-c", str(ROOT / source),
                            "-o", str(obj)], check=True)
            objects.append(obj)
        subprocess.run(["clang++", *common, *map(str, objects), "-o", str(cls.exe)], check=True)

    def check_case(self, name):
        subprocess.run([str(self.exe), name], check=True, timeout=60)

    def test_inert_idle_real_decoder_resets_and_y_planes(self):
        self.check_case("normal")

    def test_three_coded_geometries_exact_replay_invalid_rects_and_actual_decoder_budget(self):
        self.check_case("geometry")

    def test_four_picture_floors_are_replay_frozen_admitted_and_reject_invalid_options(self):
        self.check_case("picture-styles")

    def test_each_actual_allocation_failure_parks_before_reclaim(self):
        self.check_case("allocations")

    def test_cancelled_start_and_deferred_publication(self):
        self.check_case("startup")

    def test_cache_lease_live_heaps_and_outstanding_ingress(self):
        self.check_case("reserves")

    def test_output_and_callback_pins_drain_without_lock_held_wait(self):
        self.check_case("pins")

    def test_termination_timeout_guard_and_unknown_abi_quarantine(self):
        self.check_case("quarantine")

    def test_sticky_wakeups_deadline_renewal_and_static_event_rollback(self):
        self.check_case("wakeups")

    def test_stop_waits_for_paused_private_preparation_without_freeing_it(self):
        self.check_case("preparation")

    def test_control_replies_replays_guarded_start_and_deferred_expiry_reaping(self):
        self.check_case("controls")

    def test_control_pages_fit_smaller_query_budgets_without_reexecuting_start(self):
        self.check_case("control-paging")

    def test_heap_free_replies_refusal_rings_and_read_only_clock_brackets(self):
        self.check_case("receive-evidence")

    def test_compact_credits_bind_each_lens_token_and_preserve_frozen_replays(self):
        self.check_case("credits")

    def test_bounded_diagnostics_freeze_heaps_and_pool_stack_outside_locks(self):
        self.check_case("diagnostics")

    def test_stop_invalidates_controls_before_claim_and_during_preparation(self):
        self.check_case("control-preparation")

    def test_status_reports_constructor_refusal_and_unknown_abi_quarantine(self):
        self.check_case("control-failures")

    def test_owned_nal_slots_bound_borrowed_input_and_rollback_partial_start(self):
        self.check_case("nal-input")

    def test_ordered_real_decoder_consumes_owned_nals_and_counts_actual_pictures(self):
        self.check_case("consumer")

    def test_failure_checksums_history_and_allocation_survive_owner_reclaim(self):
        self.check_case("failure-history")

    def test_owned_pictures_use_independent_fixed_clock_presenter_and_drain(self):
        self.check_case("queued-consumer")

    def test_phone_pace_reanchors_coordinator_catches_up_and_left_ignores(self):
        self.check_case("phone-pacing")

    def test_timed_nal_records_feed_presenters_without_separate_frame_time(self):
        self.check_case("timed-consumer")

    def test_timed_nal_record_publishes_frame_time_and_nal_together_or_neither(self):
        self.check_case("timed-input")

    def test_dither_y8_admission_paired_modes_default_restart_and_refusal(self):
        self.check_case("dither-consumer")

    def test_both_presenter_sides_process_posted_wire_records_and_copies(self):
        self.check_case("paired-consumer")

    def test_pause_waits_for_paired_copy_boundary_and_resume_reanchors(self):
        self.check_case("playback-controls")

    def test_seek_preroll_is_decoded_but_not_shown_and_preview_stays_paused(self):
        self.check_case("seek-controls")

    def test_pc_menu_preserves_video_and_paired_scale_is_trigger_owned(self):
        self.check_case("menu-controls")

    def test_wire_callback_only_posts_and_refuses_overflow_without_heap_or_wait(self):
        self.check_case("sync-posting")

    def test_optional_evidence_bounds_ack_waits_and_never_aborts_timing_anomalies(self):
        self.check_case("evidence")

    def test_evidence_crc_padding_replays_owner_validation_and_unread_stop(self):
        self.check_case("evidence-replays")

    def test_display_refusal_copy_completion_stop_races_and_unknown_job_quarantine(self):
        self.check_case("presentation-failures")

    def test_status_keeps_completed_copy_tuple_during_next_admission_and_timer_wrap(self):
        self.check_case("status-copy")

    def test_profile_bounded_posts_wrap_loss_stale_generation_and_frozen_pages(self):
        self.check_case("profile")

    def test_cadence_window_identity_frozen_pages_and_no_heap(self):
        self.check_case("rate-contract")

    def test_ordinary_image_overlay_lease_and_stock_fallback_remain_unchanged(self):
        self.check_case("original-display")

    def test_completed_picture_and_full_dpb_admit_optional_slots_or_retain_four(self):
        self.check_case("queue-extension")

    def test_unrecoverable_input_requires_fresh_stream_and_parameter_headers(self):
        self.check_case("recovery")

    def test_activity_and_gap_deadlines_park_without_polling_or_replay_renewal(self):
        self.check_case("inactivity")

    def test_upstream_cleanup_keeps_effects_and_defers_video_reclaim(self):
        self.check_case("cleanup")

    def test_service_notifications_never_wait_or_free_and_ignore_old_generations(self):
        self.check_case("lease-notifications")

    def test_radio_options_replay_reserved_bits_refusal_and_partial_start_release(self):
        self.check_case("radio-contract")

    def test_policy_selection_does_not_switch_live_and_latches_next_generation(self):
        self.check_case("policy-contract")

    def test_retired_gray_control_is_refused_without_mutating_playback(self):
        self.check_case("gray-contract")

    def test_each_ingress_and_direct_owner_requests_phy_but_wire_peer_does_not(self):
        self.check_case("radio-provenance")

    def test_host_disconnect_posts_without_waiting_and_cancels_only_its_generation(self):
        self.check_case("radio-disconnect")

    def test_stock_handback_waits_for_park_and_preserves_newer_output(self):
        self.check_case("handback")
