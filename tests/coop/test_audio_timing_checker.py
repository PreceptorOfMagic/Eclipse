"""Counter validation must reject continuous device starvation, even at a tiny queue."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("timing", Path(__file__).resolve().parents[2] /
                                            "scripts/coop/check_audio_timing.py")
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)


def session(starved=False, rejected=False, clocked=True):
    lines = ["[COOP-AUD.output] device-paced block_frames=240"] if clocked else []
    for i in range(3):
        # One backend lifetime; startup has one empty callback. With starvation,
        # exactly half of subsequent demand gets real PCM. All byte ledgers balance.
        consumed = i * (960000 if starved else 1920000)
        silence = 960 + (i * 960000 if starved else 0)
        lines.append(f"[AUDIO-OUTPUT] elapsed_us={i*10000000} rate=48000 frame_bytes=4 "
                     f"callback_frames=240 callbacks={1+i*2000} submitted_bytes={consumed} "
                     f"consumed_bytes={consumed} silence_bytes={silence} cleared_bytes=0 "
                     "queued_bytes=0 peak_queued_bytes=960")
        if i:
            lines.append(f"[COOP-AUD.timing] time_us={100+i*10000000} elapsed_us={i*10000000} "
                         f"mix_frames={consumed//4} peer_received_frames={i*4800} "
                         f"peer_consumed_frames={i*4800} peer_trimmed_frames=0 peer_overflow_frames=0 "
                         "peer_link_discarded_frames=0 peer_queued_frames=0 invalid_timestamp_frames=0 "
                         "queue_age_max_us=5000 ipc_age_max_us=1000")
            if clocked:
                lines.append(f"[COOP-AUD.output] submitted_frames={consumed//4} failed=0 "
                             f"self_rejected={240 if rejected else 0} peer_rejected=0")
    return "\n".join(lines)


def pull_session():
    lines = ["[COOP-AUD.pull] callback-owned block_frames=240 rate=48000 channels=2"]
    for i in range(3):
        frames=i*480000
        lines.append(f"[AUDIO-OUTPUT] elapsed_us={i*10000000} rate=48000 frame_bytes=4 "
                     f"callback_frames=480 callbacks={i*1000} submitted_bytes={frames*4} "
                     f"consumed_bytes={frames*4} silence_bytes=0 cleared_bytes=0 queued_bytes=0 "
                     f"pull=1 pull_frames={frames} pull_short_frames=0 pull_invalid_results=0 pull_deadline_misses=0 pull_publish_skips=0")
        if i:
            # Periodic producer counts may be newer than the callback snapshot.
            accepted=frames+(240 if i==1 else 0)
            lines.append(f"[COOP-AUD.pull] final={int(i==2)} output_frames={frames} "
                         f"self_accepted={accepted} self_consumed={frames} self_queued=0 self_rejected=0 "
                         f"peer_accepted={accepted} peer_consumed={frames} peer_queued=0 "
                         f"self_missing=0 peer_missing=0 self_timed={frames} peer_timed={frames} "
                         "self_untimed=0 peer_untimed=0 self_age_max_us=8000 peer_age_max_us=9000 "
                         "self_plc=0 peer_plc=0 self_gaps=0 peer_gaps=0 diagnostic_dropped=0")
    return "\n".join(lines)


class TimingCheckerTest(unittest.TestCase):
    def test_pull_periodic_counts_are_asynchronous_final_is_exact(self):
        result=timing.check(pull_session())
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["maximum_decode_to_handoff_ms"], {"self":8,"peer":9})

    def test_pull_backend_frame_byte_disagreement_is_rejected(self):
        text=pull_session().replace("pull_frames=960000", "pull_frames=0")
        self.assertTrue(any("frames/bytes disagree" in e for e in timing.check(text)["errors"]))

    def test_pull_balanced_source_reset_is_rejected(self):
        text=pull_session().replace("self_accepted=960000", "self_accepted=240000")
        text=text.replace("self_consumed=960000", "self_consumed=240000")
        text=text.replace("self_timed=960000", "self_timed=240000")
        text=text.replace("self_missing=0 peer_missing=0 self_timed=240000",
                          "self_missing=720000 peer_missing=0 self_timed=240000")
        self.assertTrue(any("counter reset" in e for e in timing.check(text)["errors"]))

    def test_pull_elapsed_time_without_delivery_progress_is_rejected(self):
        text=pull_session().replace("960000", "480000").replace("3840000", "1920000")
        text=text.replace("callbacks=2000", "callbacks=1000")
        self.assertTrue(any("no application delivery progress" in e for e in timing.check(text)["errors"]))

    def test_pull_stationary_final_after_prior_progress_is_allowed(self):
        lines=pull_session().splitlines()
        last=lines[-1]
        lines[-1]=last.replace("final=1", "final=0")
        lines.append(lines[-2].replace("elapsed_us=20000000", "elapsed_us=20000001"))
        lines.append(last)
        self.assertEqual(timing.check("\n".join(lines))["errors"], [])

    def test_pull_callback_quantum_can_change_without_format_change(self):
        text=pull_session().replace("callback_frames=480 callbacks=2000",
                                    "callback_frames=240 callbacks=2000")
        self.assertEqual(timing.check(text)["errors"], [])
        text=text.replace("rate=48000 frame_bytes=4 callback_frames=240",
                          "rate=44100 frame_bytes=4 callback_frames=240")
        self.assertTrue(any("backend format changed" in e for e in timing.check(text)["errors"]))

    def test_pull_new_required_counters_are_not_defaulted(self):
        for field in ("self_plc=0", "peer_gaps=0", "pull_publish_skips=0"):
            with self.subTest(field=field):
                with self.assertRaises(KeyError): timing.check(pull_session().replace(field, ""))

    def test_pull_final_loss_is_rejected(self):
        text=pull_session().replace("self_accepted=960000", "self_accepted=960240")
        self.assertTrue(any("final self accepted" in e for e in timing.check(text)["errors"]))

    def test_pull_handoff_is_not_complete_with_missing_timestamps(self):
        text=pull_session().replace("self_untimed=0", "self_untimed=1")
        self.assertTrue(any("untimed delivered" in e for e in timing.check(text)["errors"]))

    def test_pull_startup_short_render_is_also_rejected(self):
        text=pull_session().replace("pull_short_frames=0", "pull_short_frames=240",1)
        self.assertTrue(any("incomplete pull" in e for e in timing.check(text)["errors"]))

    def test_pull_missing_final_is_not_conservation_evidence(self):
        text=pull_session().replace("final=1", "final=0")
        self.assertTrue(any("joined final" in e for e in timing.check(text)["errors"]))

    def test_pull_incomplete_schema_is_not_repaired(self):
        text=pull_session().replace("peer_timed=960000", "")
        with self.assertRaises(KeyError): timing.check(text)

    def test_balanced_half_rate_is_not_success(self):
        result = timing.check(session(starved=True))
        self.assertEqual(result["maximum_output_ring_ms"], 5)
        self.assertTrue(any("underrun after startup" in e for e in result["errors"]))

    def test_continuous_owner_with_startup_silence(self):
        self.assertEqual(timing.check(session())["errors"], [])

    def test_rejected_decoded_audio_fails(self):
        self.assertTrue(any("rejected decoded PCM" in e for e in
                            timing.check(session(rejected=True))["errors"]))

    def test_legacy_scope_does_not_claim_continuous_owner(self):
        result = timing.check(session(starved=True, clocked=False))
        self.assertFalse(result["device_paced"])
        self.assertEqual(result["errors"], [])


if __name__ == "__main__":
    unittest.main()
