//! RAII wrapper over the C-side Chromecast TLS session.

use super::cstring;
use super::ffi::*;
use core::ffi::{c_int, c_void};

pub(crate) struct CastTlsStream(*mut c_void);

unsafe impl Send for CastTlsStream {}

impl CastTlsStream {
    pub(crate) fn connect(host: &str, port: u16, timeout_ms: u32) -> Option<Self> {
        let host_c = cstring(host).ok()?;
        let ptr = unsafe { rock_cast_tls_open(host_c.as_ptr(), port, timeout_ms) };
        if ptr.is_null() {
            None
        } else {
            Some(Self(ptr))
        }
    }

    pub(crate) fn send(&mut self, data: &[u8], timeout_ms: u32) -> Result<(), ()> {
        let ret = unsafe { rock_cast_tls_send(self.0, data.as_ptr(), data.len(), timeout_ms) };
        if ret == data.len() as c_int {
            Ok(())
        } else {
            Err(())
        }
    }

    pub(crate) fn recv(&mut self, buf: &mut [u8], timeout_ms: u32) -> Result<usize, ()> {
        let ret = unsafe { rock_cast_tls_recv(self.0, buf.as_mut_ptr(), buf.len(), timeout_ms) };
        if ret > 0 {
            Ok(ret as usize)
        } else if ret == 0 {
            Ok(0)
        } else {
            Err(())
        }
    }
}

impl Drop for CastTlsStream {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { rock_cast_tls_close(self.0) };
            self.0 = core::ptr::null_mut();
        }
    }
}
