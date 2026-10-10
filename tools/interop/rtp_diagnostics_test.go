package main

import "testing"

func TestRTPDiagnosticsSeparateSourceAndSequenceGaps(t *testing.T) {
	var r rtpReception
	r.Observe(100, 1000)
	r.SetFrameSamples(960)
	r.Observe(101, 2920) // One unsent 20 ms slot, but transmitted sequence is contiguous.
	if s := r.Snapshot(); s.SequenceMissing != 0 || s.TimestampGapSamples != 960 || s.TimestampGapEvents != 1 {
		t.Fatalf("source skip counted as missing RTP: %+v", s)
	}
	r.Observe(103, 4840) // Packet 102 has not arrived; timestamp attribution is ambiguous.
	if s := r.Snapshot(); s.SequenceMissing != 1 || s.SequenceGapEvents != 1 || s.TimestampGapSamples != 960 {
		t.Fatalf("sequence gap attribution wrong: %+v", s)
	}
	r.Observe(102, 3880) // Late arrival fills the remembered hole, without moving the timestamp baseline.
	r.Observe(104, 5800)
	r.Observe(104, 5800) // Duplicate must not create either kind of gap.
	if s := r.Snapshot(); s.SequenceMissing != 0 || s.Reordered != 1 || s.Duplicates != 1 || s.TimestampGapSamples != 960 || s.TimestampDiscontinuities != 0 {
		t.Fatalf("late/duplicate packet inflated gaps: %+v", s)
	}
}

func TestRTPDiagnosticsWrapAndBoundedLateHistory(t *testing.T) {
	var r rtpReception
	r.Observe(65535, ^uint32(0)-959)
	r.Observe(0, 0)
	if s := r.Snapshot(); s.SequenceMissing != 0 || s.TimestampGapSamples != 0 || s.TimestampDiscontinuities != 0 {
		t.Fatalf("RTP wrap produced a false gap: %+v", s)
	}
	r.Observe(2, 1920)
	for seq := uint16(3); seq < 260; seq++ {
		r.Observe(seq, uint32(seq)*960)
	}
	r.Observe(1, 960) // Too old to repair a missing entry in the 256-packet window.
	if s := r.Snapshot(); s.SequenceMissing != 1 || s.Reordered != 1 || s.TimestampGapSamples != 0 {
		t.Fatalf("bounded old arrival corrupted counters: %+v", s)
	}
}
