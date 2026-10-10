//! Bounded QUIC datagram fragmentation. Reassembly never emits a partial frame.
use std::{
    collections::BTreeMap,
    time::{Duration, Instant},
};

pub const MAX_FRAME: usize = 8 * 1024 * 1024;
pub const HEADER: usize = 24;
const MAGIC: u32 = 0x31474452; // RDG1
const MAX_IN_FLIGHT: usize = 3;
const TTL: Duration = Duration::from_millis(120);

pub fn fragment(frame: &[u8], sequence: u64, mtu: usize) -> Result<Vec<Vec<u8>>, String> {
    if frame.len() < 32 || frame.len() > MAX_FRAME {
        return Err("invalid video frame length".into());
    }
    if mtu <= HEADER {
        return Err("QUIC datagrams unavailable or MTU too small".into());
    }
    let stride = (mtu - HEADER).min(u16::MAX as usize);
    let count = frame.len().div_ceil(stride);
    if count > 16384 {
        return Err("video fragmentation count exceeds bound".into());
    }
    let mut out = Vec::with_capacity(count);
    for (index, payload) in frame.chunks(stride).enumerate() {
        let mut packet = Vec::with_capacity(HEADER + payload.len());
        packet.extend_from_slice(&MAGIC.to_le_bytes());
        packet.extend_from_slice(&sequence.to_le_bytes());
        packet.extend_from_slice(&(index as u16).to_le_bytes());
        packet.extend_from_slice(&(count as u16).to_le_bytes());
        packet.extend_from_slice(&(frame.len() as u32).to_le_bytes());
        packet.extend_from_slice(&(stride as u16).to_le_bytes());
        packet.extend_from_slice(&0u16.to_le_bytes());
        packet.extend_from_slice(payload);
        out.push(packet);
    }
    Ok(out)
}

struct Assembly {
    data: Vec<u8>,
    present: Vec<bool>,
    received: usize,
    stride: usize,
    created: Instant,
}
pub struct Reassembler {
    frames: BTreeMap<u64, Assembly>,
    pub lost: bool,
}
impl Reassembler {
    pub fn new() -> Self {
        Self {
            frames: BTreeMap::new(),
            lost: false,
        }
    }
    pub fn expire(&mut self, now: Instant) {
        self.frames.retain(|_, assembly| {
            if now.duration_since(assembly.created) > TTL {
                self.lost = true;
                false
            } else {
                true
            }
        });
    }
    pub fn push(&mut self, packet: &[u8], now: Instant) -> Result<Option<(u64, Vec<u8>)>, String> {
        self.expire(now);
        if packet.len() <= HEADER || u32::from_le_bytes(packet[0..4].try_into().unwrap()) != MAGIC {
            return Err("invalid video datagram header".into());
        }
        let sequence = u64::from_le_bytes(packet[4..12].try_into().unwrap());
        let index = u16::from_le_bytes(packet[12..14].try_into().unwrap()) as usize;
        let count = u16::from_le_bytes(packet[14..16].try_into().unwrap()) as usize;
        let total = u32::from_le_bytes(packet[16..20].try_into().unwrap()) as usize;
        let stride = u16::from_le_bytes(packet[20..22].try_into().unwrap()) as usize;
        if !(32..=MAX_FRAME).contains(&total)
            || stride == 0
            || count == 0
            || count > 16384
            || index >= count
            || count != total.div_ceil(stride)
        {
            return Err("video datagram exceeds assembly bounds".into());
        }
        let offset = index * stride;
        let expected = stride.min(total - offset);
        if packet.len() != HEADER + expected {
            return Err("invalid video datagram length".into());
        }
        if !self.frames.contains_key(&sequence) {
            if self.frames.len() >= MAX_IN_FLIGHT {
                if let Some(oldest) = self.frames.keys().next().copied() {
                    self.frames.remove(&oldest);
                    self.lost = true;
                }
            }
            self.frames.insert(
                sequence,
                Assembly {
                    data: vec![0; total],
                    present: vec![false; count],
                    received: 0,
                    stride,
                    created: now,
                },
            );
        }
        let assembly = self.frames.get_mut(&sequence).unwrap();
        if assembly.data.len() != total
            || assembly.stride != stride
            || assembly.present.len() != count
        {
            return Err("inconsistent video fragment metadata".into());
        }
        if !assembly.present[index] {
            assembly.data[offset..offset + expected].copy_from_slice(&packet[HEADER..]);
            assembly.present[index] = true;
            assembly.received += 1;
        } else if assembly.data[offset..offset + expected] != packet[HEADER..] {
            return Err("conflicting duplicate video fragment".into());
        }
        if assembly.received == count {
            let complete = self.frames.remove(&sequence).unwrap();
            Ok(Some((sequence, complete.data)))
        } else {
            Ok(None)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn shuffled_duplicates_round_trip() {
        let frame: Vec<_> = (0..64000).map(|v| (v % 251) as u8).collect();
        let packets = fragment(&frame, 42, 1200).unwrap();
        let mut assembly = Reassembler::new();
        let now = Instant::now();
        let mut result = None;
        for packet in packets.iter().rev() {
            result = assembly.push(packet, now).unwrap().or(result);
            if result.is_none() {
                assert!(assembly.push(packet, now).unwrap().is_none());
            }
        }
        assert_eq!(result, Some((42, frame)));
    }
    #[test]
    fn truncation_and_bounds_rejected() {
        let mut packets = fragment(&vec![7; 2048], 9, 1200).unwrap();
        let mut a = Reassembler::new();
        packets[0].pop();
        assert!(a.push(&packets[0], Instant::now()).is_err());
        assert!(a.push(&[0; 20], Instant::now()).is_err());
        assert!(fragment(&vec![0; MAX_FRAME + 1], 0, 1200).is_err());
    }
    #[test]
    fn expired_frame_signals_loss() {
        let packets = fragment(&vec![2; 4096], 1, 1200).unwrap();
        let now = Instant::now();
        let mut a = Reassembler::new();
        a.push(&packets[0], now).unwrap();
        a.expire(now + TTL + Duration::from_millis(1));
        assert!(a.lost);
        assert!(a.frames.is_empty());
    }
    #[test]
    fn conflict_rejected() {
        let packets = fragment(&vec![3; 4096], 1, 1200).unwrap();
        let now = Instant::now();
        let mut a = Reassembler::new();
        a.push(&packets[0], now).unwrap();
        let mut conflict = packets[0].clone();
        conflict[HEADER] ^= 1;
        assert!(a.push(&conflict, now).is_err());
    }
}
