//! The image decoders' C functions (`crate::imgdec`; was `src/apps/imgdec.c`): each decodes
//! straight into a canvas made by img_canvas_new (src/apps/imgdec.c), which applies the
//! size limits.

use core::ffi::{c_char, c_int, c_void};
use crate::imgdec::{self, Sink};

extern "C" {
    fn img_canvas_new(w: c_int, h: c_int, px: *mut *mut u32) -> *mut c_void;
    fn canvas_free(c: *mut c_void);
}

struct CanvasSink { canvas: *mut c_void }
impl Sink for CanvasSink {
    fn begin(&mut self, w: u32, h: u32) -> Option<&mut [u32]> {
        unsafe {
            if !self.canvas.is_null() { canvas_free(self.canvas); self.canvas = core::ptr::null_mut(); }
            let mut px: *mut u32 = core::ptr::null_mut();
            let c = img_canvas_new(w as c_int, h as c_int, &mut px);
            if c.is_null() || px.is_null() { return None; }
            self.canvas = c;
            Some(core::slice::from_raw_parts_mut(px, w as usize * h as usize))
        }
    }
}

unsafe fn run(d: *const u8, n: u32, err: *mut c_char, ecap: c_int, f: fn(&[u8], &mut dyn Sink) -> imgdec::R) -> *mut c_void {
    let data = if d.is_null() || n == 0 { &[][..] } else { core::slice::from_raw_parts(d, n as usize) };
    let mut s = CanvasSink { canvas: core::ptr::null_mut() };
    match f(data, &mut s) {
        Ok(()) => s.canvas,
        Err(m) => {
            if !s.canvas.is_null() { canvas_free(s.canvas); }
            if !m.is_empty() { super::put_cstr(err, ecap, m); }
            core::ptr::null_mut()
        }
    }
}

/// kind: 1 PNG, 2 GIF, 3 JPEG; a canvas, or NULL with the reason in err (which may be empty)
#[no_mangle]
pub unsafe extern "C" fn img_decode_rs(kind: c_int, d: *const u8, n: u32, err: *mut c_char, ecap: c_int) -> *mut c_void {
    let f: fn(&[u8], &mut dyn Sink) -> imgdec::R = match kind { 1 => imgdec::png, 2 => imgdec::gif, _ => imgdec::jpeg };
    run(d, n, err, ecap, f)
}
/// canvas_t *bmp_load(const uint8_t *d, uint32_t n): 24/32-bit BMP (Paint, image_decode)
#[no_mangle]
pub unsafe extern "C" fn bmp_load(d: *const u8, n: u32) -> *mut c_void {
    let mut e = [0 as c_char; 2];
    run(d, n, e.as_mut_ptr(), 2, imgdec::bmp)
}
