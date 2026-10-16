use serde::{Deserialize, Serialize};

#[derive(Serialize)]
pub struct PairingCreate<'a> {
    pub device_display_name: &'a str,
    pub device_type: &'static str,
    pub app_version: &'a str,
}

#[derive(Deserialize)]
pub struct PairingRequest {
    pub pairing_request_id: String,
    pub desktop_token: String,
    pub approval_secret: String,
    pub short_code: String,
    pub verification_phrase: String,
}

impl PairingRequest {
    pub fn deep_link(&self, base_url: &str) -> String {
        format!(
            "{}/?code={}#secret={}",
            base_url.trim_end_matches('/'),
            self.short_code,
            self.approval_secret
        )
    }
}

#[derive(Serialize)]
pub struct PairingComplete<'a> {
    pub desktop_token: &'a str,
}

#[derive(Deserialize)]
pub struct Credentials {
    pub device_id: String,
    pub device_secret: String,
    pub access_token: String,
    pub access_expires_at: String,
}

#[derive(Deserialize)]
pub struct SessionToken {
    pub access_token: String,
    pub access_expires_at: String,
}

#[derive(Serialize)]
pub struct DeviceSession<'a> {
    pub device_id: &'a str,
    pub device_secret: &'a str,
}

pub fn qr_matrix(link: &str) -> Result<(u16, Vec<u8>), qrcode::types::QrError> {
    let code = qrcode::QrCode::new(link.as_bytes())?;
    let width = code.width() as u16;
    let modules = code
        .into_colors()
        .into_iter()
        .map(|c| u8::from(c == qrcode::Color::Dark))
        .collect();
    Ok((width, modules))
}

#[unsafe(no_mangle)]
pub extern "C" fn rock_qr_encode(
    text: *const core::ffi::c_char,
    out_buf: *mut u8,
    out_cap: usize,
    out_width: *mut u16,
) -> core::ffi::c_int {
    if text.is_null() || out_buf.is_null() || out_width.is_null() {
        return -1;
    }
    let cstr = unsafe { core::ffi::CStr::from_ptr(text) };
    let text_str = match cstr.to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };
    match qr_matrix(text_str) {
        Ok((width, modules)) => {
            if modules.len() > out_cap {
                return -2;
            }
            unsafe {
                core::ptr::copy_nonoverlapping(modules.as_ptr(), out_buf, modules.len());
                *out_width = width;
            }
            0
        }
        Err(_) => -3,
    }
}

fn days_from_civil(mut y: i32, m: u32, d: u32) -> i64 {
    y -= if m <= 2 { 1 } else { 0 };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = (y - era * 400) as u32;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    (era as i64) * 146097 + (doe as i64) - 719468
}

pub fn parse_rfc3339_to_ms(s: &str) -> Option<i64> {
    let s = s.trim();
    if s.len() < 19 {
        return None;
    }
    let b = s.as_bytes();
    if b[4] != b'-'
        || b[7] != b'-'
        || (b[10] != b'T' && b[10] != b't')
        || b[13] != b':'
        || b[16] != b':'
    {
        return None;
    }
    let year: i32 = s[0..4].parse().ok()?;
    let month: u32 = s[5..7].parse().ok()?;
    let day: u32 = s[8..10].parse().ok()?;
    let hour: i64 = s[11..13].parse().ok()?;
    let min: i64 = s[14..16].parse().ok()?;
    let sec: i64 = s[17..19].parse().ok()?;

    if !(1..=12).contains(&month)
        || !(1..=31).contains(&day)
        || !(0..=23).contains(&hour)
        || !(0..=59).contains(&min)
        || !(0..=60).contains(&sec)
    {
        return None;
    }

    let days = days_from_civil(year, month, day);
    let mut total_sec = days * 86_400 + hour * 3600 + min * 60 + sec;

    let mut ms = 0i64;
    let mut rest = &s[19..];
    if rest.starts_with('.') || rest.starts_with(',') {
        let frac_end = rest[1..]
            .find(|c: char| !c.is_ascii_digit())
            .map(|idx| idx + 1)
            .unwrap_or(rest.len());
        let frac_str = &rest[1..frac_end];
        let mut frac_val = 0i64;
        for (i, ch) in frac_str.chars().take(3).enumerate() {
            let digit = (ch as u8 - b'0') as i64;
            let mult = match i {
                0 => 100,
                1 => 10,
                _ => 1,
            };
            frac_val += digit * mult;
        }
        ms = frac_val;
        rest = &rest[frac_end..];
    }

    if rest.is_empty() || rest == "Z" || rest == "z" {
        // UTC
    } else if (rest.starts_with('+') || rest.starts_with('-')) && rest.len() >= 3 {
        let sign = if rest.starts_with('+') { 1i64 } else { -1i64 };
        let offset_str = &rest[1..];
        let (tz_hour, tz_min) = if let Some(colon) = offset_str.find(':') {
            let h: i64 = offset_str[..colon].parse().ok()?;
            let m: i64 = offset_str[colon + 1..].parse().ok()?;
            (h, m)
        } else if offset_str.len() == 2 {
            let h: i64 = offset_str.parse().ok()?;
            (h, 0)
        } else if offset_str.len() == 4 {
            let h: i64 = offset_str[..2].parse().ok()?;
            let m: i64 = offset_str[2..4].parse().ok()?;
            (h, m)
        } else {
            return None;
        };
        let offset_sec = sign * (tz_hour * 3600 + tz_min * 60);
        total_sec -= offset_sec;
    } else {
        return None;
    }

    Some(total_sec * 1000 + ms)
}

pub fn should_fetch(
    cached_expires_at: Option<&str>,
    now_rfc3339: &str,
    min_remaining_ms: u64,
) -> bool {
    let Some(expires_at) = cached_expires_at else {
        return true;
    };
    let Some(expires_ms) = parse_rfc3339_to_ms(expires_at) else {
        return true;
    };
    let Some(now_ms) = parse_rfc3339_to_ms(now_rfc3339) else {
        return true;
    };
    if expires_ms <= now_ms {
        return true;
    }
    let remaining_ms = (expires_ms - now_ms) as u64;
    remaining_ms <= min_remaining_ms
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rfc3339_parsing_basic_and_subseconds() {
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:00Z"), Some(0));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01Z"), Some(1000));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01.5Z"), Some(1500));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01.54Z"), Some(1540));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01.123Z"), Some(1123));
        assert_eq!(
            parse_rfc3339_to_ms("1970-01-01T00:00:01.123456Z"),
            Some(1123)
        );
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T01:00:00+01:00"), Some(0));
        assert_eq!(
            parse_rfc3339_to_ms("1970-01-01T00:00:00-05:00"),
            Some(5 * 3600 * 1000)
        );
    }

    #[test]
    fn fresh_token_does_not_fetch() {
        let now = "2026-10-15T23:00:00Z";
        let expires_at = "2026-10-15T23:15:00Z"; // 15 min = 900_000 ms remaining
        assert!(!should_fetch(Some(expires_at), now, 60_000));

        // With fractional microseconds from server
        let expires_with_us = "2026-10-15T23:15:00.654321Z";
        assert!(!should_fetch(Some(expires_with_us), now, 60_000));
    }

    #[test]
    fn threshold_min_remaining_triggers_fetch() {
        let now = "2026-10-15T23:14:00Z";
        let expires_exact = "2026-10-15T23:15:00Z"; // exactly 60_000 ms remaining
        assert!(should_fetch(Some(expires_exact), now, 60_000));

        let expires_under = "2026-10-15T23:14:59Z"; // 59_000 ms remaining
        assert!(should_fetch(Some(expires_under), now, 60_000));

        let expires_above = "2026-10-15T23:15:01Z"; // 61_000 ms remaining
        assert!(!should_fetch(Some(expires_above), now, 60_000));
    }

    #[test]
    fn expired_token_triggers_fetch() {
        let now = "2026-10-15T23:16:00Z";
        let expires_past = "2026-10-15T23:15:00Z"; // expired 1 min ago
        assert!(should_fetch(Some(expires_past), now, 60_000));

        let expires_equal = "2026-10-15T23:16:00Z"; // exactly now
        assert!(should_fetch(Some(expires_equal), now, 60_000));
    }

    #[test]
    fn missing_cache_triggers_fetch() {
        let now = "2026-10-15T23:00:00Z";
        assert!(should_fetch(None, now, 60_000));
    }

    #[test]
    fn malformed_timestamps_trigger_fetch() {
        let valid = "2026-10-15T23:00:00Z";
        assert!(should_fetch(Some("garbage"), valid, 60_000));
        assert!(should_fetch(Some(valid), "garbage", 60_000));
        assert!(should_fetch(Some("2026-99-99T99:99:99Z"), valid, 60_000));
    }
}

