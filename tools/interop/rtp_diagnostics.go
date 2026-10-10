package main

import "sync/atomic"

const rtpHistoryPackets = 256

type rtpReceptionSnapshot struct {
	SequenceMissing, SequenceGapEvents, Reordered, Duplicates         uint64
	TimestampGapSamples, TimestampGapEvents, TimestampDiscontinuities uint64
}

// One TrackRemote reader owns the history. Atomic counters allow status reads.
// Remembered sequence gaps can be filled by late packets; gaps older than this
// bounded window remain inferred missing, not proof of radio packet loss.
type rtpReception struct {
	initialized      bool
	highestSequence  uint16
	highestIndex     int64
	highestTimestamp uint32
	previousSamples  int
	history          [rtpHistoryPackets]struct {
		index    int64
		received bool
	}
	missing, gapEvents, reordered, duplicates                         atomic.Uint64
	timestampGapSamples, timestampGapEvents, timestampDiscontinuities atomic.Uint64
}

func (r *rtpReception) Observe(sequence uint16, timestamp uint32) bool {
	if !r.initialized {
		r.initialized, r.highestSequence, r.highestIndex, r.highestTimestamp = true, sequence, 1, timestamp
		r.previousSamples = wavFrameSamples
		r.history[1].index, r.history[1].received = 1, true
		return true
	}
	delta := int64(int16(sequence - r.highestSequence))
	index := r.highestIndex + delta
	if delta <= 0 {
		if index > 0 && r.history[index%rtpHistoryPackets].index == index {
			slot := &r.history[index%rtpHistoryPackets]
			if slot.received {
				r.duplicates.Add(1)
			} else {
				r.missing.Add(^uint64(0))
				r.reordered.Add(1)
				slot.received = true
			}
		} else {
			r.reordered.Add(1)
		}
		return false
	}
	if delta > 1 {
		r.missing.Add(uint64(delta - 1))
		r.gapEvents.Add(1)
		for missing := max(r.highestIndex+1, index-rtpHistoryPackets+1); missing < index; missing++ {
			slot := &r.history[missing%rtpHistoryPackets]
			slot.index, slot.received = missing, false
		}
	} else {
		// A contiguous RTP sequence makes a source-timeline gap distinguishable
		// from a missing sequence. Across sequence gaps, unknown packet duration
		// prevents attributing a timestamp jump separately to unsent frames.
		gap := int64(int32(timestamp-r.highestTimestamp)) - int64(r.previousSamples)
		if gap > 0 {
			r.timestampGapSamples.Add(uint64(gap))
			r.timestampGapEvents.Add(1)
		}
		if gap < 0 {
			r.timestampDiscontinuities.Add(1)
		}
	}
	r.highestSequence, r.highestIndex, r.highestTimestamp = sequence, index, timestamp
	r.previousSamples = wavFrameSamples // Fallback if this packet cannot decode.
	slot := &r.history[index%rtpHistoryPackets]
	slot.index, slot.received = index, true
	return true
}

func (r *rtpReception) SetFrameSamples(samples int) { r.previousSamples = samples }

func (r *rtpReception) Snapshot() rtpReceptionSnapshot {
	return rtpReceptionSnapshot{SequenceMissing: r.missing.Load(), SequenceGapEvents: r.gapEvents.Load(),
		Reordered: r.reordered.Load(), Duplicates: r.duplicates.Load(), TimestampGapSamples: r.timestampGapSamples.Load(),
		TimestampGapEvents: r.timestampGapEvents.Load(), TimestampDiscontinuities: r.timestampDiscontinuities.Load()}
}
