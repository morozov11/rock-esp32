//! Hand-rolled protobuf (proto2) for CASTV2 CastMessage / DeviceAuthMessage.
//!
//! Ported from rockcast with zero external proto library dependencies.

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProtoError {
    Truncated,
    BadVarint,
    Utf8,
    MissingPayload,
}

impl core::fmt::Display for ProtoError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::Truncated => write!(f, "incomplete protobuf"),
            Self::BadVarint => write!(f, "invalid varint"),
            Self::Utf8 => write!(f, "invalid UTF-8 in protobuf field"),
            Self::MissingPayload => write!(f, "expected STRING or BINARY payload"),
        }
    }
}

impl std::error::Error for ProtoError {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PayloadType {
    String = 0,
    Binary = 1,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CastMessage {
    pub source_id: String,
    pub destination_id: String,
    pub namespace: String,
    pub payload: Payload,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Payload {
    String(String),
    Binary(Vec<u8>),
}

impl CastMessage {
    pub fn string(
        source_id: impl Into<String>,
        destination_id: impl Into<String>,
        namespace: impl Into<String>,
        json: impl Into<String>,
    ) -> Self {
        Self {
            source_id: source_id.into(),
            destination_id: destination_id.into(),
            namespace: namespace.into(),
            payload: Payload::String(json.into()),
        }
    }

    pub fn binary(
        source_id: impl Into<String>,
        destination_id: impl Into<String>,
        namespace: impl Into<String>,
        bytes: Vec<u8>,
    ) -> Self {
        Self {
            source_id: source_id.into(),
            destination_id: destination_id.into(),
            namespace: namespace.into(),
            payload: Payload::Binary(bytes),
        }
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(128);
        // protocol_version = CASTV2_1_0 (0), field 1 varint
        write_key(1, 0, &mut out);
        write_varint(0, &mut out);

        write_string(2, &self.source_id, &mut out);
        write_string(3, &self.destination_id, &mut out);
        write_string(4, &self.namespace, &mut out);

        match &self.payload {
            Payload::String(s) => {
                write_key(5, 0, &mut out);
                write_varint(PayloadType::String as u64, &mut out);
                write_string(6, s, &mut out);
            }
            Payload::Binary(b) => {
                write_key(5, 0, &mut out);
                write_varint(PayloadType::Binary as u64, &mut out);
                write_bytes(7, b, &mut out);
            }
        }
        out
    }

    pub fn encode_framed(&self) -> Vec<u8> {
        let payload = self.encode();
        let len = payload.len() as u32;
        let mut framed = Vec::with_capacity(4 + payload.len());
        framed.extend_from_slice(&len.to_be_bytes());
        framed.extend_from_slice(&payload);
        framed
    }

    pub fn decode(buf: &[u8]) -> Result<Self, ProtoError> {
        let mut i = 0;
        let mut source_id = String::new();
        let mut destination_id = String::new();
        let mut namespace = String::new();
        let mut payload_type = PayloadType::String;
        let mut payload_utf8: Option<String> = None;
        let mut payload_binary: Option<Vec<u8>> = None;

        while i < buf.len() {
            let (key, ni) = read_varint(buf, i)?;
            i = ni;
            let field = (key >> 3) as u32;
            let wire = (key & 0x7) as u32;

            match (field, wire) {
                (1, 0) => {
                    let (_v, ni) = read_varint(buf, i)?;
                    i = ni;
                }
                (2, 2) => {
                    let (s, ni) = read_bytes(buf, i)?;
                    source_id = String::from_utf8(s).map_err(|_| ProtoError::Utf8)?;
                    i = ni;
                }
                (3, 2) => {
                    let (s, ni) = read_bytes(buf, i)?;
                    destination_id = String::from_utf8(s).map_err(|_| ProtoError::Utf8)?;
                    i = ni;
                }
                (4, 2) => {
                    let (s, ni) = read_bytes(buf, i)?;
                    namespace = String::from_utf8(s).map_err(|_| ProtoError::Utf8)?;
                    i = ni;
                }
                (5, 0) => {
                    let (v, ni) = read_varint(buf, i)?;
                    payload_type = if v == 1 {
                        PayloadType::Binary
                    } else {
                        PayloadType::String
                    };
                    i = ni;
                }
                (6, 2) => {
                    let (s, ni) = read_bytes(buf, i)?;
                    payload_utf8 = Some(String::from_utf8(s).map_err(|_| ProtoError::Utf8)?);
                    i = ni;
                }
                (7, 2) => {
                    let (b, ni) = read_bytes(buf, i)?;
                    payload_binary = Some(b);
                    i = ni;
                }
                (_, 0) => {
                    let (_v, ni) = read_varint(buf, i)?;
                    i = ni;
                }
                (_, 1) => {
                    if i + 8 > buf.len() {
                        return Err(ProtoError::Truncated);
                    }
                    i += 8;
                }
                (_, 2) => {
                    let (_b, ni) = read_bytes(buf, i)?;
                    i = ni;
                }
                (_, 5) => {
                    if i + 4 > buf.len() {
                        return Err(ProtoError::Truncated);
                    }
                    i += 4;
                }
                _ => return Err(ProtoError::BadVarint),
            }
        }

        let payload = match payload_type {
            PayloadType::String => Payload::String(payload_utf8.ok_or(ProtoError::MissingPayload)?),
            PayloadType::Binary => {
                Payload::Binary(payload_binary.ok_or(ProtoError::MissingPayload)?)
            }
        };

        Ok(Self {
            source_id,
            destination_id,
            namespace,
            payload,
        })
    }
}

/// DeviceAuthMessage { challenge: AuthChallenge {} } -> field 1, empty length-delimited.
pub fn encode_auth_challenge() -> Vec<u8> {
    let mut out = Vec::new();
    write_key(1, 2, &mut out);
    write_varint(0, &mut out);
    out
}

fn write_key(field: u32, wire: u32, out: &mut Vec<u8>) {
    write_varint(((field as u64) << 3) | u64::from(wire), out);
}

fn write_varint(mut v: u64, out: &mut Vec<u8>) {
    while v >= 0x80 {
        out.push((v as u8) | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
}

fn write_string(field: u32, s: &str, out: &mut Vec<u8>) {
    write_bytes(field, s.as_bytes(), out);
}

fn write_bytes(field: u32, bytes: &[u8], out: &mut Vec<u8>) {
    write_key(field, 2, out);
    write_varint(bytes.len() as u64, out);
    out.extend_from_slice(bytes);
}

fn read_varint(buf: &[u8], mut i: usize) -> Result<(u64, usize), ProtoError> {
    let mut result = 0u64;
    let mut shift = 0u32;
    loop {
        if i >= buf.len() {
            return Err(ProtoError::Truncated);
        }
        let b = buf[i];
        i += 1;
        result |= u64::from(b & 0x7f) << shift;
        if b & 0x80 == 0 {
            return Ok((result, i));
        }
        shift += 7;
        if shift > 63 {
            return Err(ProtoError::BadVarint);
        }
    }
}

fn read_bytes(buf: &[u8], i: usize) -> Result<(Vec<u8>, usize), ProtoError> {
    let (len, i) = read_varint(buf, i)?;
    let len = len as usize;
    if i + len > buf.len() {
        return Err(ProtoError::Truncated);
    }
    Ok((buf[i..i + len].to_vec(), i + len))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_string() {
        let msg = CastMessage::string(
            "sender-0",
            "receiver-0",
            "urn:x-cast:com.google.cast.tp.heartbeat",
            r#"{"type":"PING"}"#,
        );
        let encoded = msg.encode();
        let decoded = CastMessage::decode(&encoded).unwrap();
        assert_eq!(decoded.source_id, "sender-0");
        assert_eq!(decoded.destination_id, "receiver-0");
        assert_eq!(decoded.namespace, "urn:x-cast:com.google.cast.tp.heartbeat");
        match decoded.payload {
            Payload::String(s) => assert!(s.contains("PING")),
            _ => panic!("expected string"),
        }
    }

    #[test]
    fn roundtrip_binary() {
        let msg = CastMessage::binary(
            "sender-0",
            "receiver-0",
            "urn:x-cast:com.google.cast.tp.deviceauth",
            vec![1, 2, 3, 4],
        );
        let encoded = msg.encode();
        let decoded = CastMessage::decode(&encoded).unwrap();
        assert_eq!(decoded.payload, Payload::Binary(vec![1, 2, 3, 4]));
    }

    #[test]
    fn auth_challenge_encoding() {
        let challenge = encode_auth_challenge();
        assert_eq!(challenge, vec![0x0a, 0x00]);
    }
}
