//! Owly control plane for the kernel AWDL netdev; no IP payload crosses this ABI.
use owly_awdl::{Action, Channel, Config, Input, MacAddr, Node, Presence, RadioConstraints, Time};
use std::ffi::{CStr, c_char, c_void};
type Emit = unsafe extern "C" fn(*mut c_void, u32, *const u8, usize) -> i32;
pub struct Control {
    channels: [u8; 2],
    name: String,
    node: Node,
    target: Option<MacAddr>,
    peer: Option<MacAddr>,
    emit: Emit,
    context: *mut c_void,
    deadline: Time,
    window: (u64, u64),
    tx_drain_us: u64,
    plan: Option<(i64, [u8; 16])>,
    applied: Option<[u8; 28]>,
    observed: u64,
    available: u64,
    report: u64,
    vht_peers: Vec<(MacAddr, PeerCaps)>,
    announced: Vec<MacAddr>,
    local_he: Option<[u8; 21]>,
}
#[derive(Default, Clone)]
struct PeerCaps {
    clock: Option<owly_awdl::sync::Clock>,
    caps: [u8; 28],
    ht: bool,
    vht: bool,
    he: Option<Vec<u8>>,
    apple_he: Option<bool>,
    local_he: Option<[u8; 21]>,
    he_state: &'static str,
    wire: Vec<u8>,
    observed_containers: Vec<Vec<u8>>,
}
impl PeerCaps {
    fn ready(&self) -> bool {
        self.ht && self.vht && self.caps[4] & 3 != 3
    }
    fn merge(&mut self, delta: Self) {
        if delta.clock.is_some() {
            self.clock = delta.clock;
        }
        if delta.ht {
            self.caps[12..20].copy_from_slice(&delta.caps[12..20]);
            self.ht = true;
        }
        if delta.vht {
            self.caps[..12].copy_from_slice(&delta.caps[..12]);
            self.vht = true;
        }
        if delta.apple_he.is_some() {
            self.apple_he = delta.apple_he;
            self.local_he = delta.local_he;
        }
        if delta.he.is_some() {
            self.he = delta.he;
            self.he_state = delta.he_state;
            self.caps[20..].copy_from_slice(&delta.caps[20..]);
        }
        // Apple AWDL advertises HE through Data State, using VHT to bound NSS.
        if self.he.is_none() && self.apple_he.is_some() {
            self.caps[20..].fill(0);
            if self.apple_he == Some(true) && self.vht {
                if let Some(local) = self.local_he {
                    let mut peer = local;
                    // The Apple boolean does not advertise optional 1x/4x LTF GI modes.
                    peer[7] &= !64;
                    peer[13] &= !4;
                    let vht = u16::from_le_bytes([self.caps[4], self.caps[5]]);
                    let mut rx = u16::from_le_bytes([local[17], local[18]]);
                    for nss in 0..8 {
                        if (vht >> (nss * 2)) & 3 == 3 {
                            rx |= 3 << (nss * 2);
                        }
                    }
                    peer[17..19].copy_from_slice(&rx.to_le_bytes());
                    peer[12] &= !128;
                    peer[15] = (peer[15] & 63) | 128;
                    self.he_state = match he80(&local, &peer) {
                        Ok(he) => {
                            self.caps[20..].copy_from_slice(&he);
                            "apple-data-state"
                        }
                        Err(reason) => reason,
                    };
                }
            }
        }
    }
}
impl Control {
    fn emit(&self, kind: u32, data: &[u8]) -> i32 {
        unsafe { (self.emit)(self.context, kind, data.as_ptr(), data.len()) }
    }
    fn input(&mut self, input: Input<'_>, now: Time) {
        if self.peer.is_some() {
            let begin = self.observed.max(self.window.0);
            let end = now.0.min(self.window.1);
            self.available += end.saturating_sub(begin);
            if now.0.saturating_sub(self.report) >= 1_048_576 {
                println!(
                    "AWDL_SCHEDULE elapsed_us={} available_us={}",
                    now.0, self.available
                );
                self.report = now.0;
            }
        }
        self.observed = now.0;
        let actions: Vec<_> = self.node.handle(input).collect();
        for action in actions {
            match action {
                Action::Transmit { mut frame, .. } => {
                    if frame.bytes[0] == 0xd0 {
                        // The countdown describes target_tx; account for time
                        // spent processing the received synchronization frame.
                        frame.bytes[32..36].copy_from_slice(&(now.0 as u32).to_le_bytes());
                        advertise_vht80(&mut frame.bytes);
                        if let Some(he) = self.local_he {
                            advertise_he80(&mut frame.bytes, &he);
                        }
                        if self.channels[1] != 0
                            && let Some(mac) = self.peer
                        {
                            if let Some(seq) = self.node.peer(mac).and_then(|p| p.sequence) {
                                advertise_follow(&mut frame.bytes, seq.channels());
                            }
                        }
                        self.emit(0, &frame.bytes);
                    }
                }
                Action::ArmTimer(at) => self.deadline = at,
                Action::PeerUp(info) => {
                    println!(
                        "{}",
                        serde_json::json!({"event":"peer-observed","mac":format_mac(info.mac),"name":info.name,"class":info.devclass})
                    );
                    if !self.name.is_empty() && info.name.eq_ignore_ascii_case(&self.name) {
                        self.target = Some(info.mac);
                    }
                }
                Action::PeerDown(mac) => {
                    println!(
                        "{}",
                        serde_json::json!({"event":"peer-down","mac":format_mac(mac)})
                    );
                    self.vht_peers.retain(|(addr, _)| *addr != mac);
                    self.announced.retain(|addr| *addr != mac);
                    if self.peer == Some(mac) {
                        self.emit(4, &mac.0);
                        self.peer = None;
                        self.applied = None;
                    }
                }
                _ => {}
            }
        }
        for peer in self.node.peers() {
            if peer.valid
                && !self.announced.contains(&peer.mac)
                && self
                    .vht_peers
                    .iter()
                    .any(|(mac, caps)| *mac == peer.mac && caps.ready())
            {
                println!(
                    "{}",
                    serde_json::json!({"event":"peer","mac":format_mac(peer.mac),"name":peer.name,"class":peer.devclass})
                );
                self.announced.push(peer.mac);
            }
        }
        if self.peer.is_none() && !self.name.is_empty() {
            self.target = self
                .node
                .peers()
                .find(|p| {
                    p.valid
                        && self
                            .vht_peers
                            .iter()
                            .any(|(mac, caps)| *mac == p.mac && caps.ready())
                        && (self.name == "*"
                            || p.name.eq_ignore_ascii_case(&self.name)
                            || format_mac(p.mac).eq_ignore_ascii_case(&self.name))
                })
                .map(|p| p.mac);
            if let Some(mac) = self.target {
                let caps = self
                    .vht_peers
                    .iter()
                    .find(|(addr, _)| *addr == mac)
                    .unwrap()
                    .1
                    .caps;
                let mut data = [0u8; 34];
                data[..6].copy_from_slice(&mac.0);
                data[6..].copy_from_slice(&caps);
                if self.node.peer(mac).is_some_and(|p| p.valid) && self.emit(1, &data) == 0 {
                    self.peer = Some(mac);
                    self.applied = Some(caps);
                }
            }
        }
        if let Some(peer) = self.peer {
            let caps = self
                .vht_peers
                .iter()
                .find(|(mac, _)| *mac == peer)
                .map(|(_, p)| p.caps);
            if let Some(caps) = caps {
                if self.applied != Some(caps) && self.emit(7, &caps) == 0 {
                    self.applied = Some(caps);
                    println!(
                        "AWDL_PHY_APPLIED peer={} he={} he_map={:04x}",
                        format_mac(peer),
                        caps[20],
                        u16::from_le_bytes([caps[21], caps[22]])
                    );
                }
            }
            // A selected peer can follow a different synchronization master.
            let clock = self
                .vht_peers
                .iter()
                .find(|(mac, _)| *mac == peer)
                .and_then(|(_, caps)| caps.clock.as_ref())
                .unwrap_or_else(|| self.node.clock());
            let boundary = now.0 + clock.next_slot_us(now);
            if let Some(seq) = self.node.peer(peer).and_then(|p| p.sequence) {
                let slot = clock.slot(now);
                let channels = std::array::from_fn(|i| match seq.channel((slot + i) % 16) {
                    channel if self.channels.contains(&channel) => channel,
                    _ => 0,
                });
                let base = boundary as i64 - 65536;
                let changed = plan_changed(self.plan.as_ref(), base, &channels);
                if changed {
                    let mut data = [0u8; 26];
                    data[..8].copy_from_slice(&base.to_le_bytes());
                    data[8..24].copy_from_slice(&channels);
                    data[24] = channels[15];
                    data[25] = channels[0];
                    if self.emit(6, &data) == 0 {
                        self.plan = Some((base, channels));
                    }
                }
                let same_before = seq.channel((slot + 15) % 16) == channels[0];
                let same_after = seq.channel((slot + 1) % 16) == channels[0];
                let window = if channels[0] == 0 {
                    (0, 0)
                } else {
                    (
                        (base + if same_before { 0 } else { 3072 }).max(1) as u64,
                        boundary - if same_after { 0 } else { self.tx_drain_us },
                    )
                };
                if self.channels[1] == 0 && window != self.window {
                    let mut data = [0u8; 16];
                    data[..8].copy_from_slice(&window.0.to_le_bytes());
                    data[8..].copy_from_slice(&window.1.to_le_bytes());
                    self.emit(5, &data);
                }
                self.window = window;
            }
            self.deadline = Time(self.deadline.0.min(boundary));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn control_new(
    mac: *const u8,
    channel: u8,
    secondary_channel: u8,
    name: *const c_char,
    local_name: *const c_char,
    he: *const u8,
    tx_drain_us: u32,
    emit: Emit,
    context: *mut c_void,
) -> *mut Control {
    let local = MacAddr(
        unsafe { std::slice::from_raw_parts(mac, 6) }
            .try_into()
            .unwrap(),
    );
    let mut cfg = Config::new(
        local,
        unsafe { CStr::from_ptr(local_name) }
            .to_string_lossy()
            .as_ref(),
        RadioConstraints {
            channel: Channel(channel),
            presence: Presence::Fixed,
        },
    );
    // Kernel owns BA negotiation and native data aggregation.
    cfg.no_ack = false;
    cfg.accept_addba = false;
    cfg.amsdu = true;
    cfg.neighbour_echo = false;
    cfg.rssi_threshold = None;
    Box::into_raw(Box::new(Control {
        channels: [channel, secondary_channel],
        name: unsafe { CStr::from_ptr(name) }
            .to_string_lossy()
            .into_owned(),
        node: Node::new(cfg, Time(1)),
        target: None,
        peer: None,
        applied: None,
        emit,
        context,
        deadline: Time(1),
        window: (0, 0),
        tx_drain_us: u64::from(tx_drain_us),
        plan: None,
        observed: 1,
        available: 0,
        report: 1,
        vht_peers: Vec::new(),
        announced: Vec::new(),
        local_he: if he.is_null() {
            None
        } else {
            Some(
                unsafe { std::slice::from_raw_parts(he, 21) }
                    .try_into()
                    .unwrap(),
            )
        },
    }))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn control_tick(p: *mut Control, now: u64) -> u64 {
    let p = unsafe { &mut *p };
    p.input(Input::Timer(Time(now)), Time(now));
    p.deadline.0
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn control_rx(p: *mut Control, data: *const u8, len: usize, now: u64) {
    let p = unsafe { &mut *p };
    let data = unsafe { std::slice::from_raw_parts(data, len) };
    let reference = action_reference_time(data, Time(now));
    if let Some((mac, delta)) = parse_caps(data, p.local_he.as_ref(), reference) {
        let index = match p.vht_peers.iter().position(|(addr, _)| *addr == mac) {
            Some(index) => index,
            None => {
                p.vht_peers.push((mac, PeerCaps::default()));
                p.vht_peers.len() - 1
            }
        };
        let peer = &mut p.vht_peers[index].1;
        if !delta.wire.is_empty()
            && peer.observed_containers.len() < 4
            && !peer.observed_containers.contains(&delta.wire)
        {
            println!(
                "AWDL_CAPS_WIRE peer={} subtype={} containers={}",
                format_mac(mac),
                data[30],
                delta
                    .wire
                    .iter()
                    .map(|b| format!("{b:02x}"))
                    .collect::<String>()
            );
            peer.observed_containers.push(delta.wire.clone());
        }
        let old = (peer.caps, peer.ready(), peer.he.clone());
        peer.merge(delta);
        if old != (peer.caps, peer.ready(), peer.he.clone()) {
            let raw = peer
                .he
                .as_ref()
                .map(|v| v.iter().map(|b| format!("{b:02x}")).collect::<String>())
                .unwrap_or_default();
            println!(
                "AWDL_CAPS peer={} subtype={} ht={} vht={} he_state={} he_raw={}",
                format_mac(mac),
                data[30],
                peer.ht,
                peer.vht,
                if peer.he.is_none() && peer.apple_he != Some(true) {
                    "absent"
                } else {
                    peer.he_state
                },
                raw
            );
            if peer.ready() {
                let caps = &peer.caps;
                let map = u16::from_le_bytes([caps[4], caps[5]]);
                println!(
                    "AWDL_PHY peer={} vht_map={map:04x} he={} he_map={:04x} ldpc={} gi={} padding={}/{}/{}",
                    format_mac(mac),
                    caps[20],
                    u16::from_le_bytes([caps[21], caps[22]]),
                    caps[23],
                    caps[24],
                    caps[25],
                    caps[26],
                    caps[27]
                );
            }
        }
    }
    p.input(
        Input::Frame {
            rx_time: reference,
            rssi: None,
            bytes: data,
        },
        Time(now),
    );
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn control_free(p: *mut Control) {
    drop(unsafe { Box::from_raw(p) });
}
fn advertise_vht80(frame: &mut Vec<u8>) {
    if frame.len() < 40 || frame[0] != 0xd0 {
        return;
    }
    let mut at = 40;
    while at + 3 <= frame.len() {
        let mut len = u16::from_le_bytes([frame[at + 1], frame[at + 2]]) as usize;
        if at + 3 + len > frame.len() {
            return;
        }
        if frame[at] == 7 && len >= 8 {
            if len == 8 {
                frame.insert(at + 9, 0xff);
                len = 9;
                frame[at + 1..at + 3].copy_from_slice(&9u16.to_le_bytes());
            }
            frame[at + 5] |= 0x62;
            frame[at + 6] |= 8;
            frame[at + 7] |= 3;
            frame[at + 8] = 0xff;
            frame[at + 9] = 0xff;
        }
        at += 3 + len;
    }
    // VHT80, two streams MCS0-9, short GI, 7991-byte MPDU, 64-KiB A-MPDU.
    frame.extend_from_slice(&[
        17, 14, 0, 191, 12, 0x21, 0, 0x80, 1, 0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0,
    ]);
}
fn plan_changed(old: Option<&(i64, [u8; 16])>, base: i64, channels: &[u8; 16]) -> bool {
    old.is_none_or(|(start, previous)| {
        let delta = base - *start;
        let slots = (delta + 32768).div_euclid(65536);
        slots < 0
            || slots >= 8
            || (delta - slots * 65536).abs() > 1024
            || (0..16).any(|i| channels[i] != previous[(i + slots as usize) % 16])
    })
}
fn advertise_follow(frame: &mut [u8], channels: [u8; 16]) {
    let mut at = 40;
    while at + 3 <= frame.len() {
        let len = u16::from_le_bytes([frame[at + 1], frame[at + 2]]) as usize;
        if at + 3 + len > frame.len() {
            return;
        }
        let offset = match frame[at] {
            4 if len >= 71 => Some(33),
            18 if len >= 38 => Some(0),
            _ => None,
        };
        if let Some(offset) = offset {
            let v = &mut frame[at + 3 + offset..at + 3 + offset + 38];
            v[..6].copy_from_slice(&[15, 3, 0, 3, 255, 255]);
            for (i, c) in channels.iter().enumerate() {
                v[6 + i * 2] = if *c == 149 || *c == 161 { *c } else { 0 };
                v[7 + i * 2] = 0x80;
            }
        }
        at += 3 + len;
    }
}

// IO80211AWDLPeer::parseAwdlDataPathTLVAndTakeAction maps extended bit 8
// to setHeSupported; fillInCacheParams passes that flag to the WLAN driver.
fn data_state_ext(value: &[u8]) -> Option<usize> {
    let flags = u16::from_le_bytes(value.get(..2)?.try_into().ok()?);
    let mut at = 2;
    for (flag, len) in [(0x100, 3), (0x200, 2), (1, 8), (2, 6), (4, 6), (0x10, 2)] {
        if flags & flag != 0 {
            value.get(at..at + len)?;
            at += len;
        }
    }
    if flags & 0x1000 != 0 {
        let len = u16::from_le_bytes(value.get(at..at + 2)?.try_into().ok()?) as usize;
        at += 2;
        value.get(at..at + len)?;
        at += len;
    }
    if flags & 0x8000 == 0 {
        return None;
    }
    value.get(at..at + 2)?;
    Some(at)
}

fn advertise_he80(frame: &mut Vec<u8>, local: &[u8; 21]) {
    if local[6] & 4 == 0 || frame.len() < 40 {
        return;
    }
    let mut at = 40;
    while at + 3 <= frame.len() {
        let len = u16::from_le_bytes([frame[at + 1], frame[at + 2]]) as usize;
        let Some(value) = frame.get(at + 3..at + 3 + len) else {
            return;
        };
        if frame[at] == 12 {
            if let Some(offset) = data_state_ext(value) {
                frame[at + 3 + offset + 1] |= 1;
            }
        }
        at += 3 + len;
    }
}

fn he80(local: &[u8; 21], peer: &[u8]) -> Result<[u8; 8], &'static str> {
    if peer.len() < 21 {
        return Err("short");
    }
    if peer[6] & local[6] & 4 == 0 {
        return Err("no-he80");
    }
    let mut map = 0xffffu16;
    let tx = u16::from_le_bytes([local[19], local[20]]);
    let rx = u16::from_le_bytes([peer[17], peer[18]]);
    for i in 0..2 {
        let a = (tx >> (2 * i)) & 3;
        let b = (rx >> (2 * i)) & 3;
        if a != 3 && b != 3 {
            map = (map & !(3 << (2 * i))) | (a.min(b) << (2 * i));
        }
    }
    if map & 3 == 3 {
        return Err("no-common-mcs");
    }
    let streams = if (map >> 2) & 3 == 3 { 1 } else { 2 };
    let end = 21 + if peer[6] & 8 != 0 { 4 } else { 0 } + if peer[6] & 16 != 0 { 4 } else { 0 };
    if peer.len() < end {
        return Err("short-mcs");
    }
    let mut padding = [peer[15] >> 6; 3];
    if peer[12] & 128 != 0 {
        let ppe = &peer[end..];
        let header = *ppe.first().ok_or("short-ppe")?;
        let nss = (header & 7) as usize + 1;
        let ru = (header >> 3) & 15;
        if nss < streams
            || ru == 0
            || ppe.len() < (7 + 6 * nss * ru.count_ones() as usize).div_ceil(8)
        {
            return Err("invalid-ppe");
        }
        padding = [2; 3];
        for (j, p) in padding.iter_mut().enumerate() {
            if ru & (1 << j) != 0 {
                *p = 0;
            }
        }
        let bits = |bit: usize| -> u8 {
            (0..3)
                .map(|i| ((ppe[(bit + i) / 8] >> ((bit + i) % 8)) & 1) << i)
                .sum()
        };
        let mut bit = 7;
        for i in 0..nss {
            for j in 0..4 {
                if ru & (1 << j) != 0 {
                    let need = if bits(bit) != 7 {
                        2
                    } else if bits(bit + 3) != 7 {
                        1
                    } else {
                        0
                    };
                    bit += 6;
                    if i < streams && j < 3 {
                        padding[j] = padding[j].max(need);
                    }
                }
            }
        }
    } else if padding[0] == 3 {
        return Err("invalid-ppe");
    }
    Ok([
        1,
        map as u8,
        (map >> 8) as u8,
        u8::from(peer[7] & local[7] & 32 != 0),
        u8::from(peer[7] & local[7] & 64 != 0) | (u8::from(peer[13] & local[13] & 4 != 0) << 1),
        padding[0],
        padding[1],
        padding[2],
    ])
}

// AWDL's down-counter is sampled at target_tx. The sender fills phy_tx
// when the frame actually leaves its queue; both counters wrap at 32 bits.
fn action_reference_time(frame: &[u8], received: Time) -> Time {
    if frame.len() < 40 || frame[0] != 0xd0 || frame[24..30] != [0x7f, 0, 0x17, 0xf2, 8, 0x10] {
        return received;
    }
    let phy_tx = u32::from_le_bytes(frame[32..36].try_into().unwrap());
    let target_tx = u32::from_le_bytes(frame[36..40].try_into().unwrap());
    received.minus(u64::from(phy_tx.wrapping_sub(target_tx)))
}

fn parse_caps(frame: &[u8], local_he: Option<&[u8; 21]>, now: Time) -> Option<(MacAddr, PeerCaps)> {
    if frame.len() < 40 || frame[0] != 0xd0 || frame[24..30] != [0x7f, 0, 0x17, 0xf2, 8, 0x10] {
        return None;
    }
    let mut at = 40;
    let mut parsed = PeerCaps::default();
    let caps = &mut parsed.caps;
    while at + 3 <= frame.len() {
        let len = u16::from_le_bytes([frame[at + 1], frame[at + 2]]) as usize;
        let value = frame.get(at + 3..at + 3 + len)?;
        if frame[at] == 4 {
            let sync = owly_awdl::tlv::SyncParams::parse(value).ok()?;
            let mut clock = owly_awdl::sync::Clock::new(now);
            clock.update(now, sync.next_tu, sync.aw_counter);
            parsed.clock = Some(clock);
        }
        if frame[at] == 12 {
            if let Some(offset) = data_state_ext(value) {
                parsed.apple_he = Some(value[offset + 1] & 1 != 0);
                parsed.local_he = local_he.copied();
                // Retain the variable-length Data State for native wire attribution.
                let count = (3 + len).min(512usize.saturating_sub(parsed.wire.len()));
                parsed.wire.extend_from_slice(&frame[at..at + count]);
            }
        }
        if frame[at] == 17 {
            let count = (3 + len).min(512usize.saturating_sub(parsed.wire.len()));
            parsed.wire.extend_from_slice(&frame[at..at + count]);
            let mut ie = 0;
            while ie < value.len() {
                let id = *value.get(ie)?;
                let size = *value.get(ie + 1)? as usize;
                let body = value.get(ie + 2..ie + 2 + size)?;
                if id == 191 && size == 12 {
                    caps[..12].copy_from_slice(body);
                    parsed.vht = true;
                }
                if id == 255 && body.first() == Some(&35) {
                    parsed.he = Some(body[1..].to_vec());
                    parsed.he_state = match local_he {
                        None => "local-unavailable",
                        Some(_) if body.len() < 22 => "short",
                        Some(local) if body[7] & local[6] & 4 == 0 => "no-he80",
                        Some(local) => match he80(local, &body[1..]) {
                            Ok(he) => {
                                caps[20..].copy_from_slice(&he);
                                "valid"
                            }
                            Err(reason) => reason,
                        },
                    };
                }
                ie += 2 + size;
            }
        }
        if frame[at] == 7 && value.len() >= 8 {
            caps[12..20].copy_from_slice(&value[..8]);
            if value.len() == 8 {
                caps[18] = 0;
            }
            parsed.ht = true;
        }
        at += 3 + len;
    }
    // Apple action frames may end in two zero padding bytes.
    if at != frame.len() && frame.get(at..) != Some(&[0, 0][..]) {
        return None;
    }
    Some((MacAddr(frame[10..16].try_into().ok()?), parsed))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn control_select(p: *mut Control, name: *const c_char) -> i32 {
    let p = unsafe { &mut *p };
    if p.peer.is_some() {
        return -16;
    }
    p.name = unsafe { CStr::from_ptr(name) }
        .to_string_lossy()
        .into_owned();
    0
}
fn format_mac(mac: MacAddr) -> String {
    mac.0
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(":")
}
