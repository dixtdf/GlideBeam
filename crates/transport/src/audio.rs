//! A single low-latency PCM block per unreliable datagram. Never retransmit old sound.
use std::{
    collections::VecDeque,
    time::{Duration, Instant},
};

pub const MAGIC: u32 = 0x31414652; // RFA1
pub const HEADER_BYTES: usize = 32;
pub const SAMPLE_RATE: u32 = 48_000;
pub const CHANNELS: u16 = 2;
pub const FRAMES: u16 = 240; // 5 ms
pub const PACKET_BYTES: usize = HEADER_BYTES + FRAMES as usize * CHANNELS as usize * 2;
// ScreenCaptureKit delivers source batches: 1024 frames can produce five
// 240-frame packets at once. Capacity absorbs that burst; wall-clock TTL below
// still discards packets that spend 20 ms waiting for network/consumer progress.
pub const QUEUE_PACKETS: usize = 8;
pub const MAX_AGE: Duration = Duration::from_millis(20);
const DISCONTINUITY: u32 = 1;

pub fn valid_packet(packet: &[u8]) -> bool {
    packet.len() == PACKET_BYTES
        && u32::from_le_bytes(packet[0..4].try_into().unwrap()) == MAGIC
        && u32::from_le_bytes(packet[4..8].try_into().unwrap()) == SAMPLE_RATE
        && u16::from_le_bytes(packet[8..10].try_into().unwrap()) == CHANNELS
        && u16::from_le_bytes(packet[10..12].try_into().unwrap()) == FRAMES
        && u32::from_le_bytes(packet[12..16].try_into().unwrap()) & !DISCONTINUITY == 0
}

pub struct Packet {
    pub data: Vec<u8>,
    pub queued: Instant,
}

#[derive(Default)]
pub struct Queue {
    packets: VecDeque<Packet>,
    discontinuity: bool,
}

impl Queue {
    pub fn expire(&mut self, now: Instant) {
        while self
            .packets
            .front()
            .is_some_and(|p| now.duration_since(p.queued) >= MAX_AGE)
        {
            self.packets.pop_front();
            self.discontinuity = true;
        }
    }

    pub fn push(&mut self, data: Vec<u8>, now: Instant) {
        self.expire(now);
        if self.packets.len() >= QUEUE_PACKETS {
            self.packets.pop_front();
            self.discontinuity = true;
        }
        self.packets.push_back(Packet { data, queued: now });
    }

    pub fn pop(&mut self, now: Instant) -> Option<Packet> {
        self.expire(now);
        let mut packet = self.packets.pop_front()?;
        if self.discontinuity {
            let flags = u32::from_le_bytes(packet.data[12..16].try_into().unwrap()) | DISCONTINUITY;
            packet.data[12..16].copy_from_slice(&flags.to_le_bytes());
            self.discontinuity = false;
        }
        Some(packet)
    }

    pub fn front_len(&mut self, now: Instant) -> Option<usize> {
        self.expire(now);
        self.packets.front().map(|p| p.data.len())
    }

    pub fn len(&mut self, now: Instant) -> usize {
        self.expire(now);
        self.packets.len()
    }

    pub fn mark_discontinuity(&mut self) {
        self.discontinuity = true;
    }
}

#[cfg(test)]
pub fn test_packet(sequence: u64) -> Vec<u8> {
    let mut packet = vec![0; PACKET_BYTES];
    packet[..4].copy_from_slice(&MAGIC.to_le_bytes());
    packet[4..8].copy_from_slice(&SAMPLE_RATE.to_le_bytes());
    packet[8..10].copy_from_slice(&CHANNELS.to_le_bytes());
    packet[10..12].copy_from_slice(&FRAMES.to_le_bytes());
    packet[16..24].copy_from_slice(&sequence.to_le_bytes());
    for (index, sample) in packet[HEADER_BYTES..].chunks_exact_mut(2).enumerate() {
        sample.copy_from_slice(&(index as i16).to_le_bytes());
    }
    packet
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn malformed_packet_never_accepted() {
        let packet = test_packet(0);
        assert!(valid_packet(&packet));
        for len in 0..PACKET_BYTES {
            assert!(!valid_packet(&packet[..len]));
        }
        let mut extra = packet.clone();
        extra.push(0);
        assert!(!valid_packet(&extra));
        for offset in [0, 4, 8, 10, 13] {
            let mut invalid = packet.clone();
            invalid[offset] ^= 1;
            assert!(!valid_packet(&invalid));
        }
        let mut discontinuous = packet;
        discontinuous[12] = 1;
        assert!(valid_packet(&discontinuous));
    }

    #[test]
    fn bounded_queue_keeps_recent_sound_and_marks_loss() {
        let now = Instant::now();
        let mut queue = Queue::default();
        for sequence in 0..10 {
            queue.push(test_packet(sequence), now);
        }
        assert_eq!(queue.len(now), QUEUE_PACKETS);
        let newest = queue.pop(now).unwrap();
        assert_eq!(
            u64::from_le_bytes(newest.data[16..24].try_into().unwrap()),
            10 - QUEUE_PACKETS as u64
        );
        assert_eq!(newest.data[12], 1);
        assert_eq!(queue.pop(now).unwrap().data[12], 0);
    }

    #[test]
    fn capture_batch_does_not_drop_its_fifth_packet() {
        let now = Instant::now();
        let mut queue = Queue::default();
        for sequence in 0..5 {
            queue.push(test_packet(sequence), now);
        }
        for sequence in 0..5 {
            let packet = queue.pop(now).unwrap();
            assert_eq!(
                u64::from_le_bytes(packet.data[16..24].try_into().unwrap()),
                sequence
            );
            assert_eq!(packet.data[12], 0);
        }
        assert_eq!(queue.len(now), 0);
    }

    #[test]
    fn expired_sound_cannot_resurface_after_a_stall() {
        let now = Instant::now();
        let mut queue = Queue::default();
        queue.push(test_packet(1), now);
        assert!(queue.pop(now + MAX_AGE).is_none());
        queue.push(test_packet(2), now + MAX_AGE);
        assert_eq!(queue.pop(now + MAX_AGE).unwrap().data[12], 1);
        assert_eq!(queue.len(now + MAX_AGE), 0);
    }
}
