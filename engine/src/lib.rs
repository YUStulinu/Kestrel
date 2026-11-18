//! Kestrel: a browser engine. HTML parsing, CSS, layout and painting live here;
//! JavaScript (C++), image decoding (Zig) and networking (Go) are linked in or spawned.
pub mod css;
pub mod font;
pub mod html;
pub mod js;
pub mod layout;
pub mod paint;
pub mod render;
