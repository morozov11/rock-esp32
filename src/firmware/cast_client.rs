//! Chromecast client implementation managing TLS transport and DMR session.

use super::platform::{self, CastTlsStream};
use crate::device_control::cast::*;
use crate::device_control::cast_proto::{CastMessage, Payload};
use serde_json::Value;

pub(crate) struct CastClient {
    stream: Option<CastTlsStream>,
    inbox: Vec<u8>,
    req_id: u32,
    receiver_id: Option<String>,
    receiver_name: Option<String>,
    receiver: Option<DiscoveredReceiver>,
    transport_id: Option<String>,
    session_id: Option<String>,
    media_session_id: Option<i64>,
    last_ping: u64,
}

impl CastClient {
    pub(crate) fn new() -> Self {
        Self {
            stream: None,
            inbox: Vec::with_capacity(4096),
            req_id: 1,
            receiver_id: None,
            receiver_name: None,
            receiver: None,
            transport_id: None,
            session_id: None,
            media_session_id: None,
            last_ping: 0,
        }
    }

    pub(crate) fn is_connected(&self) -> bool {
        self.stream.is_some() && self.transport_id.is_some()
    }

    pub(crate) fn receiver_id(&self) -> Option<&str> {
        self.receiver_id.as_deref()
    }

    #[allow(dead_code)]
    pub(crate) fn receiver_name(&self) -> Option<&str> {
        self.receiver_name.as_deref()
    }

    fn next_req(&mut self) -> u32 {
        let cur = self.req_id;
        self.req_id = self.req_id.wrapping_add(1).max(1);
        cur
    }

    fn send_msg(stream: &mut CastTlsStream, msg: &CastMessage) -> Result<(), ()> {
        let frame = msg.encode_framed();
        stream.send(&frame, 3000)
    }

    /// Receiver keepalive PING detection (phase-4 dedup of five hand-rolled
    /// blocks).
    fn is_ping(msg: &CastMessage) -> bool {
        msg.namespace == NS_HEARTBEAT
            && matches!(&msg.payload, Payload::String(s) if s.contains("PING"))
    }

    /// PONG back to the ping's source. Best-effort by design (phase 5
    /// swallowed-error policy): a lost PONG surfaces later as a pump error,
    /// which the session handles through the reconnect path.
    fn send_pong(stream: &mut CastTlsStream, msg: &CastMessage) {
        let _ = Self::send_msg(stream, &msg_pong_to(&msg.source_id));
    }

    /// Wait for a receiver-status message carrying a running DMR session
    /// (phase-4 dedup of the pre-LAUNCH reuse probe and the post-LAUNCH
    /// wait, which differ only in budget). Answers PINGs while waiting.
    fn wait_dmr_session(
        stream: &mut CastTlsStream,
        inbox: &mut Vec<u8>,
        budget_ms: u64,
    ) -> Result<Option<AppSession>, ()> {
        let start_wait = platform::uptime_ms();
        while platform::uptime_ms() - start_wait < budget_ms {
            if let Some(msg) = Self::read_msg(stream, inbox, 1000)? {
                if Self::is_ping(&msg) {
                    Self::send_pong(stream, &msg);
                    continue;
                }
                if msg.namespace == NS_RECEIVER {
                    if let Payload::String(ref s) = msg.payload {
                        if let Ok(v) = serde_json::from_str::<Value>(s) {
                            if let Some(app) = parse_dmr(&v) {
                                return Ok(Some(app));
                            }
                        }
                    }
                }
            }
        }
        Ok(None)
    }

    /// Chromecast receivers may require this post-TLS challenge before CASTV2
    /// control messages. Authentication failures are best-effort, matching the
    /// desktop client: some receivers do not reply, but still accept CASTV2.
    fn authenticate(stream: &mut CastTlsStream, inbox: &mut Vec<u8>) {
        let challenge = CastMessage::binary(
            SENDER_ID,
            RECEIVER_ID,
            NS_DEVICEAUTH,
            crate::device_control::cast_proto::encode_auth_challenge(),
        );
        if Self::send_msg(stream, &challenge).is_err() {
            platform::cast_log("Cast device-auth challenge could not be sent; continuing");
            return;
        }

        let deadline = platform::uptime_ms() + CAST_GET_STATUS_TIMEOUT_MS as u64;
        while platform::uptime_ms() < deadline {
            match Self::read_msg(stream, inbox, 1_000) {
                Ok(Some(msg)) if msg.namespace == NS_DEVICEAUTH => {
                    platform::cast_log("Cast device-auth reply received");
                    return;
                }
                Ok(Some(msg)) if Self::is_ping(&msg) => {
                    Self::send_pong(stream, &msg);
                }
                Ok(Some(msg)) if msg.namespace != NS_HEARTBEAT => {
                    platform::cast_log(&format!(
                        "Cast rx namespace='{}' before session",
                        msg.namespace
                    ));
                }
                Ok(Some(_)) => {} // non-PING heartbeat: consumed silently
                Ok(None) => {}
                Err(()) => {
                    platform::cast_log("Cast device-auth read failed; continuing");
                    return;
                }
            }
        }
        platform::cast_log("Cast device-auth reply timed out; continuing");
    }

    fn read_msg(
        stream: &mut CastTlsStream,
        inbox: &mut Vec<u8>,
        timeout_ms: u32,
    ) -> Result<Option<CastMessage>, ()> {
        let start = platform::uptime_ms();
        let mut chunk = [0u8; 1024];

        loop {
            // Check if full frame is already in inbox
            if inbox.len() >= 4 {
                let len = u32::from_be_bytes([inbox[0], inbox[1], inbox[2], inbox[3]]) as usize;
                if len > 64 * 1024 {
                    // Frame oversized: reset inbox and error out
                    inbox.clear();
                    return Err(());
                }
                if inbox.len() >= 4 + len {
                    let body = inbox[4..4 + len].to_vec();
                    inbox.drain(0..4 + len);
                    let msg = CastMessage::decode(&body).map_err(|_| ())?;
                    return Ok(Some(msg));
                }
            }

            let elapsed = (platform::uptime_ms() - start) as u32;
            if elapsed >= timeout_ms {
                return Ok(None);
            }
            let remaining = timeout_ms - elapsed;

            let n = stream.recv(&mut chunk, remaining.min(500))?;
            if n == 0 {
                // Timeout on this socket read
                let now_elapsed = (platform::uptime_ms() - start) as u32;
                if now_elapsed >= timeout_ms {
                    return Ok(None);
                }
            } else {
                inbox.extend_from_slice(&chunk[..n]);
            }
        }
    }

    pub(crate) fn connect(
        &mut self,
        receiver_id: &str,
        device: &DiscoveredReceiver,
    ) -> Result<(), ()> {
        self.disconnect();

        let mut stream =
            CastTlsStream::connect(&device.host, device.port, CAST_CONNECT_TIMEOUT_MS)
                .ok_or(())?;

        Self::authenticate(&mut stream, &mut self.inbox);

        // 1. Send CONNECT to receiver-0
        Self::send_msg(&mut stream, &msg_connect(RECEIVER_ID))?;

        // 2. Mirror the desktop client: reuse an already-running DMR before
        // issuing LAUNCH.  Repeated LAUNCH calls leave some receivers in an
        // IDLE media session which does not consume a subsequent live LOAD.
        let status_req = self.next_req();
        Self::send_msg(&mut stream, &msg_get_receiver_status(status_req))?;
        let mut app_session = Self::wait_dmr_session(&mut stream, &mut self.inbox, 500)?;

        // 3. No active DMR: launch it, then wait for its session identifiers.
        if app_session.is_none() {
            let launch_req = self.next_req();
            Self::send_msg(&mut stream, &msg_launch_dmr(launch_req))?;
            app_session = Self::wait_dmr_session(
                &mut stream,
                &mut self.inbox,
                CAST_LAUNCH_TIMEOUT_MS as u64,
            )?;
        }

        let app = app_session.ok_or(())?;

        // 4. Send CONNECT to application's transport_id
        Self::send_msg(&mut stream, &msg_connect(&app.transport_id))?;

        self.stream = Some(stream);
        self.transport_id = Some(app.transport_id);
        self.session_id = Some(app.session_id);
        self.receiver_id = Some(receiver_id.to_string());
        self.receiver_name = Some(device.name.clone());
        self.receiver = Some(device.clone());
        self.last_ping = platform::uptime_ms();

        Ok(())
    }

    pub(crate) fn load_stream(
        &mut self,
        stream_url: &str,
        title: &str,
        content: SourceContentType,
    ) -> Result<(), ()> {
        // rockcast parity (candidate_content_types): try the transport the
        // relay actually serves first, then the fallback ladder.
        let candidates = content.cast_load_candidates();
        let last = candidates[candidates.len() - 1];
        let ladder_start = platform::uptime_ms();
        for content_type in candidates {
            let elapsed = platform::uptime_ms().saturating_sub(ladder_start);
            if elapsed >= CAST_LOAD_LADDER_TIMEOUT_MS as u64 {
                platform::cast_log("Cast LOAD ladder budget exhausted");
                return Err(());
            }
            let remaining = (CAST_LOAD_LADDER_TIMEOUT_MS as u64 - elapsed) as u32;
            let attempt_timeout = remaining.min(CAST_LOAD_ATTEMPT_TIMEOUT_MS);
            match self.load_stream_attempt(stream_url, title, content_type, attempt_timeout) {
                Ok(()) => return Ok(()),
                Err(()) if content_type != last => {
                    platform::cast_log(&format!(
                        "Cast LOAD did not start for {}; retrying with next content type",
                        content_type
                    ));
                }
                Err(()) => return Err(()),
            }
        }
        // Defensive only (candidates is never empty: it always contains the
        // preferred transport): an explicit error path instead of a panic —
        // the board builds with panic=abort.
        Err(())
    }

    fn load_stream_attempt(
        &mut self,
        stream_url: &str,
        title: &str,
        content_type: &str,
        timeout_ms: u32,
    ) -> Result<(), ()> {
        let transport_id = self.transport_id.clone().ok_or(())?;
        let session_id = self.session_id.clone().ok_or(())?;
        let load_req = self.next_req();
        let nudge_req = self.next_req();
        let load_msg = msg_load_stream(
            load_req,
            &transport_id,
            &session_id,
            stream_url,
            title,
            content_type,
        );
        let stream = self.stream.as_mut().ok_or(())?;
        platform::cast_log(&format!(
            "Sending Cast LOAD req={} contentType={}",
            load_req, content_type
        ));
        Self::send_msg(stream, &load_msg)?;

        let start_wait = platform::uptime_ms();
        let mut load_ok = false;
        let mut loaded_mid: Option<i64> = None;
        let mut pending_mid: Option<i64> = None;
        let mut nudged = false;
        let mut nudge_at: Option<u64> = None;

        while platform::uptime_ms().saturating_sub(start_wait) < timeout_ms as u64 {
            // Grace period for RE-17 receiver compatibility: some hardware (commit 1cf0f4b)
            // starts live playback while remaining in IDLE without sending further status
            // messages. Once nudged, if no error has occurred for CAST_NUDGE_GRACE_MS,
            // consider playback ready.
            if !load_ok && nudged {
                if let Some(sent_at) = nudge_at {
                    if platform::uptime_ms().saturating_sub(sent_at) >= CAST_NUDGE_GRACE_MS as u64 {
                        platform::cast_log(&format!(
                            "Cast load grace period elapsed after PLAY nudge (mid={:?}); assuming ready (RE-17)",
                            pending_mid
                        ));
                        loaded_mid = pending_mid;
                        load_ok = true;
                        break;
                    }
                }
            }

            let now = platform::uptime_ms();
            let elapsed = now.saturating_sub(start_wait);
            let remaining_total = (timeout_ms as u64).saturating_sub(elapsed);
            if remaining_total == 0 {
                break;
            }
            let mut read_timeout = (remaining_total as u32).min(1000);
            if nudged {
                if let Some(sent_at) = nudge_at {
                    let elapsed_nudge = now.saturating_sub(sent_at);
                    if elapsed_nudge < CAST_NUDGE_GRACE_MS as u64 {
                        let remaining_grace = (CAST_NUDGE_GRACE_MS as u64 - elapsed_nudge) as u32;
                        read_timeout = read_timeout.min(remaining_grace).max(50);
                    }
                }
            }

            if let Some(msg) = Self::read_msg(stream, &mut self.inbox, read_timeout)? {
                // PONG addressed to the receiver (original behaviour of this
                // loop, unlike the source-addressed pong elsewhere).
                // Best-effort, see send_pong.
                if Self::is_ping(&msg) {
                    let _ = Self::send_msg(stream, &msg_pong());
                    continue;
                }
                if msg.namespace == NS_MEDIA {
                    if let Payload::String(ref s) = msg.payload {
                        if let Ok(v) = serde_json::from_str::<Value>(s) {
                            let typ = v.get("type").and_then(Value::as_str).unwrap_or("");
                            let rid = v.get("requestId").and_then(Value::as_u64).unwrap_or(0);
                            let st_opt = v.get("status").and_then(Value::as_array).and_then(|a| a.first());
                            let pstate = st_opt.and_then(|st| st.get("playerState")).and_then(Value::as_str).unwrap_or("");
                            let ireason = st_opt.and_then(|st| st.get("idleReason")).and_then(Value::as_str).unwrap_or("");
                            let mid = st_opt.and_then(|st| st.get("mediaSessionId")).and_then(Value::as_i64);
                            let content_id = st_opt.and_then(|st| st.pointer("/media/contentId")).and_then(Value::as_str);

                            platform::cast_log(&format!(
                                "Cast rx MEDIA: typ='{}' reqId={} state='{}' idle='{}' mid={:?} content_present={} content_matches_relay={}",
                                typ, rid, pstate, ireason, mid, content_id.is_some(), content_id == Some(stream_url)
                            ));

                            if matches!(typ, "LOAD_FAILED" | "LOAD_CANCELLED" | "INVALID_REQUEST" | "ERROR") {
                                if load_frame_is_reject_for(typ, rid, load_req) {
                                    platform::cast_log(&format!("Cast LOAD rejected with type={}", typ));
                                    return Err(());
                                }
                                platform::cast_log(&format!(
                                    "Ignoring Cast LOAD rejection for another request: type={} reqId={} expected={}",
                                    typ, rid, load_req
                                ));
                                continue;
                            }
                            // Only remember mediaSessionId from messages confirmed for this stream
                            let confirmed_req = rid == load_req as u64;
                            let matches_stream = content_id == Some(stream_url);
                            if mid.is_some() && (confirmed_req || matches_stream) {
                                pending_mid = mid;
                            }
                            match classify_media_status(&v, load_req, stream_url) {
                                MediaLoadStatus::Ready(mid) => {
                                    platform::cast_log(&format!("Cast LOAD ready! mid={:?}", mid));
                                    loaded_mid = mid.or(pending_mid);
                                    load_ok = true;
                                    break;
                                }
                                MediaLoadStatus::Nudge(mid) => {
                                    pending_mid = Some(mid);
                                    if !nudged {
                                        nudged = true;
                                        nudge_at = Some(platform::uptime_ms());
                                        platform::cast_log(&format!(
                                            "Cast IDLE after LOAD; sending PLAY nudge req={} mid={}",
                                            nudge_req, mid
                                        ));
                                        let nudge = msg_play(nudge_req, &transport_id, mid);
                                        let _ = Self::send_msg(stream, &nudge);
                                    }
                                }
                                MediaLoadStatus::Error(e) => {
                                    platform::cast_log(&format!("Cast LOAD error: {}", e));
                                    return Err(());
                                }
                                MediaLoadStatus::Pending => {}
                            }
                        }
                    }
                }
            }

            // Silent-pending nudge (rockcast parity): a media session exists
            // for our LOAD but never reached BUFFERING/PLAYING. Some receiver
            // firmware only starts the live pipeline after an explicit media
            // command, so mirror the desktop client's best-effort PLAY.
            if !load_ok && !nudged {
                if let Some(mid) = pending_mid {
                    if platform::uptime_ms().saturating_sub(start_wait) >= 2_000 {
                        nudged = true;
                        nudge_at = Some(platform::uptime_ms());
                        platform::cast_log(&format!(
                            "Cast silent after LOAD; sending PLAY nudge req={} mid={}",
                            nudge_req, mid
                        ));
                        // The nudge itself is best-effort: its outcome is
                        // observed through the LOAD status loop below.
                        let nudge = msg_play(nudge_req, &transport_id, mid);
                        let _ = Self::send_msg(stream, &nudge);
                    }
                }
            }
        }

        if load_ok {
            self.media_session_id = loaded_mid;
            Ok(())
        } else {
            platform::cast_log(&format!(
                "Cast LOAD timed out after {} ms (contentType={})",
                timeout_ms, content_type
            ));
            Err(())
        }
    }

    /// Teardown cluster (phase 5 swallowed-error policy): stop_media/stop/
    /// disconnect send their CLOSE/STOP frames best-effort — the socket is
    /// about to be dropped anyway, and a receiver that misses them is
    /// recovered by the next connect()'s DMR-reuse probe. No logging: the
    /// ensuing state is "no cast session", which the UI already reflects.
    pub(crate) fn stop_media(&mut self) -> Result<(), ()> {
        let req = self.next_req();
        if let (Some(transport_id), Some(mid)) = (&self.transport_id, self.media_session_id) {
            let msg = msg_stop_media(req, transport_id, mid);
            if let Some(ref mut stream) = self.stream {
                let _ = Self::send_msg(stream, &msg);
            }
        }
        self.media_session_id = None;
        Ok(())
    }

    pub(crate) fn stop(&mut self) -> Result<(), ()> {
        // Teardown cluster: best-effort sends, see stop_media's doc comment.
        let _ = self.stop_media();
        let req = self.next_req();
        let stop_receiver_msg = self.session_id.as_ref().map(|s| msg_stop_receiver(req, s));
        let close_conn_msg = msg_close_connection(RECEIVER_ID);
        let close_tid_msg = self.transport_id.as_ref().map(|tid| msg_close_connection(tid));

        if let Some(ref mut stream) = self.stream {
            if let Some(msg) = stop_receiver_msg {
                let _ = Self::send_msg(stream, &msg);
            }
            let _ = Self::send_msg(stream, &close_conn_msg);
            if let Some(msg) = close_tid_msg {
                let _ = Self::send_msg(stream, &msg);
            }
        }
        Ok(())
    }

    pub(crate) fn set_volume(&mut self, volume_0_to_100: u8) -> Result<(), ()> {
        let req = self.next_req();
        let level = (volume_0_to_100 as f32) / 100.0;
        let msg = msg_set_volume(req, level);
        if let Some(ref mut stream) = self.stream {
            Self::send_msg(stream, &msg)
        } else {
            Ok(())
        }
    }

    pub(crate) fn disconnect(&mut self) {
        // Teardown cluster: best-effort sends, see stop_media's doc comment.
        let _ = self.stop();
        self.stream = None;
        self.transport_id = None;
        self.session_id = None;
        self.media_session_id = None;
        self.receiver_id = None;
        self.receiver_name = None;
        self.receiver = None;
        self.inbox.clear();
    }

    pub(crate) fn reconnect(&mut self) -> Result<(), ()> {
        let receiver_id = self.receiver_id.clone().ok_or(())?;
        let receiver = self.receiver.clone().ok_or(())?;
        self.connect(&receiver_id, &receiver)
    }

    pub(crate) fn pump(&mut self) -> Result<(), ()> {
        let Some(ref mut stream) = self.stream else {
            return Ok(());
        };

        // Send heartbeat ping every 5 seconds to receiver and active
        // transport. Best-effort (phase 5 policy): a dead link is detected
        // by the pump's read error, which triggers reconnect — a lost ping
        // changes nothing about that outcome.
        if platform::uptime_ms() - self.last_ping >= CAST_HEARTBEAT_INTERVAL_MS {
            self.last_ping = platform::uptime_ms();
            let _ = Self::send_msg(stream, &msg_ping());
            if let Some(ref tid) = self.transport_id {
                let _ = Self::send_msg(stream, &msg_ping_to(tid));
            }
        }

        // Quick non-blocking poll of inbound packets
        while let Some(msg) = Self::read_msg(stream, &mut self.inbox, 10)? {
            if Self::is_ping(&msg) {
                Self::send_pong(stream, &msg);
            }
        }

        Ok(())
    }
}
