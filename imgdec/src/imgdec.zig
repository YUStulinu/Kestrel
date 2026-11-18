//! Kestrel's image decoders: PNG (all color types and bit depths, Adam7
//! interlacing, tRNS) with its own inflate, and baseline JPEG (Huffman,
//! integer IDCT, chroma subsampling). Exposed to Rust through a C ABI:
//!
//!   kd_decode(data, len, &w, &h) -> RGBA pixels (w*h*4 bytes) or null
//!   kd_free(pixels, len)
const std = @import("std");

const alloc = std.heap.page_allocator;

// ---------------------------------------------------------------- inflate (RFC 1951)
const Bits = struct {
    data: []const u8,
    pos: usize = 0,
    bitbuf: u32 = 0,
    bitcnt: u5 = 0,
    fn bits(self: *Bits, need: u5) !u32 {
        var val: u32 = self.bitbuf;
        var cnt: u32 = self.bitcnt;
        while (cnt < need) {
            if (self.pos >= self.data.len) return error.Truncated;
            val |= @as(u32, self.data[self.pos]) << @intCast(cnt);
            self.pos += 1;
            cnt += 8;
        }
        const n: u32 = need;
        self.bitbuf = if (n >= 32) 0 else val >> @intCast(n);
        self.bitcnt = @intCast(cnt - n);
        if (n == 0) return 0;
        return val & ((@as(u32, 1) << @intCast(n)) - 1);
    }
};

const Huff = struct {
    count: [16]u16 = @splat(0),
    symbol: [320]u16 = @splat(0),
    fn build(self: *Huff, lengths: []const u8) void {
        self.count = @splat(0);
        for (lengths) |l| self.count[l] += 1;
        self.count[0] = 0;
        var offs: [16]u16 = undefined;
        offs[1] = 0;
        var len: usize = 1;
        while (len < 15) : (len += 1) offs[len + 1] = offs[len] + self.count[len];
        for (lengths, 0..) |l, sym| {
            if (l != 0) {
                self.symbol[offs[l]] = @intCast(sym);
                offs[l] += 1;
            }
        }
    }
    fn decode(self: *const Huff, s: *Bits) !u16 {
        var code: i32 = 0;
        var first: i32 = 0;
        var index: i32 = 0;
        var len: usize = 1;
        while (len <= 15) : (len += 1) {
            code |= @intCast(try s.bits(1));
            const count: i32 = self.count[len];
            if (code - count < first) return self.symbol[@intCast(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return error.BadCode;
    }
};

const Out = struct {
    buf: []u8,
    len: usize = 0,
    fn push(self: *Out, b: u8) !void {
        if (self.len == self.buf.len) {
            const nb = try alloc.alloc(u8, @max(self.buf.len * 2, 4096));
            @memcpy(nb[0..self.len], self.buf[0..self.len]);
            if (self.buf.len > 0) alloc.free(self.buf);
            self.buf = nb;
        }
        self.buf[self.len] = b;
        self.len += 1;
    }
};

const LBASE = [_]u16{ 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const LEXT = [_]u5{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const DBASE = [_]u16{ 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const DEXT = [_]u5{ 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

fn codes(s: *Bits, out: *Out, lit: *const Huff, dist: *const Huff) !void {
    while (true) {
        const sym = try lit.decode(s);
        if (sym < 256) {
            try out.push(@intCast(sym));
        } else if (sym == 256) {
            return;
        } else {
            const li = sym - 257;
            if (li >= 29) return error.BadLength;
            const len: usize = LBASE[li] + try s.bits(LEXT[li]);
            const ds = try dist.decode(s);
            if (ds >= 30) return error.BadDistance;
            const d: usize = DBASE[ds] + try s.bits(DEXT[ds]);
            if (d > out.len) return error.BadDistance;
            var k: usize = 0;
            while (k < len) : (k += 1) try out.push(out.buf[out.len - d]);
        }
    }
}

fn inflate(data: []const u8) !Out {
    var s = Bits{ .data = data };
    var out = Out{ .buf = &[_]u8{} };
    while (true) {
        const last = try s.bits(1);
        const ty = try s.bits(2);
        switch (ty) {
            0 => {
                s.bitbuf = 0;
                s.bitcnt = 0;
                if (s.pos + 4 > data.len) return error.Truncated;
                const len: usize = @as(usize, data[s.pos]) | (@as(usize, data[s.pos + 1]) << 8);
                s.pos += 4;
                if (s.pos + len > data.len) return error.Truncated;
                for (data[s.pos .. s.pos + len]) |b| try out.push(b);
                s.pos += len;
            },
            1 => {
                var lens: [288]u8 = undefined;
                for (0..144) |i| lens[i] = 8;
                for (144..256) |i| lens[i] = 9;
                for (256..280) |i| lens[i] = 7;
                for (280..288) |i| lens[i] = 8;
                var lit = Huff{};
                lit.build(&lens);
                var dl: [30]u8 = @splat(5);
                var dist = Huff{};
                dist.build(&dl);
                try codes(&s, &out, &lit, &dist);
            },
            2 => {
                const nlen: usize = (try s.bits(5)) + 257;
                const ndist: usize = (try s.bits(5)) + 1;
                const ncode: usize = (try s.bits(4)) + 4;
                const order = [_]u8{ 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                var cl: [19]u8 = @splat(0);
                for (0..ncode) |i| cl[order[i]] = @intCast(try s.bits(3));
                var lencode = Huff{};
                lencode.build(&cl);
                var lens: [320]u8 = @splat(0);
                var idx: usize = 0;
                while (idx < nlen + ndist) {
                    const sym = try lencode.decode(&s);
                    if (sym < 16) {
                        lens[idx] = @intCast(sym);
                        idx += 1;
                    } else {
                        var rep: usize = 0;
                        var val: u8 = 0;
                        if (sym == 16) {
                            if (idx == 0) return error.BadRepeat;
                            val = lens[idx - 1];
                            rep = 3 + try s.bits(2);
                        } else if (sym == 17) {
                            rep = 3 + try s.bits(3);
                        } else {
                            rep = 11 + try s.bits(7);
                        }
                        if (idx + rep > nlen + ndist) return error.BadRepeat;
                        while (rep > 0) : (rep -= 1) {
                            lens[idx] = val;
                            idx += 1;
                        }
                    }
                }
                var lit = Huff{};
                lit.build(lens[0..nlen]);
                var dist = Huff{};
                dist.build(lens[nlen .. nlen + ndist]);
                try codes(&s, &out, &lit, &dist);
            },
            else => return error.BadBlock,
        }
        if (last == 1) break;
    }
    return out;
}

// ---------------------------------------------------------------- PNG
fn be32(b: []const u8) u32 {
    return (@as(u32, b[0]) << 24) | (@as(u32, b[1]) << 16) | (@as(u32, b[2]) << 8) | b[3];
}

fn paeth(a: i32, b: i32, c: i32) u8 {
    const p = a + b - c;
    const pa = @abs(p - a);
    const pb = @abs(p - b);
    const pc = @abs(p - c);
    if (pa <= pb and pa <= pc) return @intCast(a);
    if (pb <= pc) return @intCast(b);
    return @intCast(c);
}

const Png = struct {
    w: u32,
    h: u32,
    depth: u8,
    ctype: u8,
    palette: [256][4]u8 = @splat(.{ 0, 0, 0, 255 }),
    trns_gray: ?u16 = null,
    trns_rgb: ?[3]u16 = null,

    fn channels(self: *const Png) u32 {
        return switch (self.ctype) {
            0 => 1,
            2 => 3,
            3 => 1,
            4 => 2,
            6 => 4,
            else => 1,
        };
    }

    // reads sample i of a raw (unfiltered) row
    fn sample(self: *const Png, row: []const u8, i: usize) u16 {
        switch (self.depth) {
            16 => return (@as(u16, row[2 * i]) << 8) | row[2 * i + 1],
            8 => return row[i],
            else => {
                const d: usize = self.depth;
                const bit = i * d;
                const byte = row[bit / 8];
                const shift: u3 = @intCast(8 - d - (bit % 8));
                return (byte >> shift) & @as(u8, @intCast((@as(u16, 1) << @intCast(d)) - 1));
            },
        }
    }

    fn to8(self: *const Png, v: u16) u8 {
        return switch (self.depth) {
            16 => @intCast(v >> 8),
            8 => @intCast(v),
            4 => @intCast(v * 17),
            2 => @intCast(v * 85),
            1 => @intCast(v * 255),
            else => @intCast(v),
        };
    }

    fn put_row(self: *const Png, row: []const u8, w: u32, out: []u8, y: u32, x0: u32, dx: u32) void {
        const ch = self.channels();
        var x: u32 = 0;
        while (x < w) : (x += 1) {
            const px = x0 + x * dx;
            const o = (@as(usize, y) * self.w + px) * 4;
            const i: usize = @as(usize, x) * ch;
            switch (self.ctype) {
                0 => {
                    const g = self.sample(row, i);
                    const v = self.to8(g);
                    out[o] = v;
                    out[o + 1] = v;
                    out[o + 2] = v;
                    out[o + 3] = if (self.trns_gray != null and self.trns_gray.? == g) 0 else 255;
                },
                2 => {
                    const r = self.sample(row, i);
                    const g = self.sample(row, i + 1);
                    const b = self.sample(row, i + 2);
                    out[o] = self.to8(r);
                    out[o + 1] = self.to8(g);
                    out[o + 2] = self.to8(b);
                    out[o + 3] = 255;
                    if (self.trns_rgb) |t| {
                        if (t[0] == r and t[1] == g and t[2] == b) out[o + 3] = 0;
                    }
                },
                3 => {
                    const p = self.palette[self.sample(row, i) & 255];
                    out[o] = p[0];
                    out[o + 1] = p[1];
                    out[o + 2] = p[2];
                    out[o + 3] = p[3];
                },
                4 => {
                    const v = self.to8(self.sample(row, i));
                    out[o] = v;
                    out[o + 1] = v;
                    out[o + 2] = v;
                    out[o + 3] = self.to8(self.sample(row, i + 1));
                },
                6 => {
                    out[o] = self.to8(self.sample(row, i));
                    out[o + 1] = self.to8(self.sample(row, i + 1));
                    out[o + 2] = self.to8(self.sample(row, i + 2));
                    out[o + 3] = self.to8(self.sample(row, i + 3));
                },
                else => {},
            }
        }
    }

    // unfilters and converts one (sub-)image; returns the bytes consumed
    fn pass(self: *const Png, data: []const u8, w: u32, h: u32, out: []u8, y0: u32, dy: u32, x0: u32, dx: u32) !usize {
        if (w == 0 or h == 0) return 0;
        const ch = self.channels();
        const bits_per_px: usize = @as(usize, ch) * self.depth;
        const stride: usize = (bits_per_px * w + 7) / 8;
        const bpp: usize = @max(1, bits_per_px / 8);
        const prev = try alloc.alloc(u8, stride);
        defer alloc.free(prev);
        const cur = try alloc.alloc(u8, stride);
        defer alloc.free(cur);
        @memset(prev, 0);
        var pos: usize = 0;
        var y: u32 = 0;
        while (y < h) : (y += 1) {
            if (pos + 1 + stride > data.len) return error.Truncated;
            const f = data[pos];
            const raw = data[pos + 1 .. pos + 1 + stride];
            pos += 1 + stride;
            for (0..stride) |i| {
                const a: i32 = if (i >= bpp) cur[i - bpp] else 0;
                const b: i32 = prev[i];
                const c: i32 = if (i >= bpp) prev[i - bpp] else 0;
                const x: i32 = raw[i];
                const v: i32 = switch (f) {
                    0 => x,
                    1 => x + a,
                    2 => x + b,
                    3 => x + @divFloor(a + b, 2),
                    4 => x + paeth(a, b, c),
                    else => return error.BadFilter,
                };
                cur[i] = @intCast(@as(u32, @bitCast(v)) & 0xff);
            }
            self.put_row(cur, w, out, y0 + y * dy, x0, dx);
            @memcpy(prev, cur);
        }
        return pos;
    }
};

fn decode_png(data: []const u8, ow: *u32, oh: *u32) ![]u8 {
    if (data.len < 8 or !std.mem.eql(u8, data[0..8], "\x89PNG\r\n\x1a\n")) return error.NotPng;
    var p: usize = 8;
    var png: Png = undefined;
    var have_ihdr = false;
    var interlace: u8 = 0;
    var idat = Out{ .buf = &[_]u8{} };
    defer if (idat.buf.len > 0) alloc.free(idat.buf);
    while (p + 8 <= data.len) {
        const len = be32(data[p .. p + 4]);
        const ty = data[p + 4 .. p + 8];
        if (p + 12 + len > data.len) return error.Truncated;
        const body = data[p + 8 .. p + 8 + len];
        if (std.mem.eql(u8, ty, "IHDR")) {
            png = Png{ .w = be32(body[0..4]), .h = be32(body[4..8]), .depth = body[8], .ctype = body[9] };
            interlace = body[12];
            have_ihdr = true;
        } else if (std.mem.eql(u8, ty, "PLTE")) {
            var i: usize = 0;
            while (i * 3 + 2 < body.len and i < 256) : (i += 1) png.palette[i] = .{ body[3 * i], body[3 * i + 1], body[3 * i + 2], 255 };
        } else if (std.mem.eql(u8, ty, "tRNS")) {
            if (png.ctype == 3) {
                for (body, 0..) |a, i| if (i < 256) {
                    png.palette[i][3] = a;
                };
            } else if (png.ctype == 0 and body.len >= 2) {
                png.trns_gray = (@as(u16, body[0]) << 8) | body[1];
            } else if (png.ctype == 2 and body.len >= 6) {
                png.trns_rgb = .{ (@as(u16, body[0]) << 8) | body[1], (@as(u16, body[2]) << 8) | body[3], (@as(u16, body[4]) << 8) | body[5] };
            }
        } else if (std.mem.eql(u8, ty, "IDAT")) {
            for (body) |b| try idat.push(b);
        } else if (std.mem.eql(u8, ty, "IEND")) {
            break;
        }
        p += 12 + len;
    }
    if (!have_ihdr or idat.len < 2) return error.NoData;
    if (png.w == 0 or png.h == 0 or png.w > 16384 or png.h > 16384) return error.BadSize;
    const raw_out = try inflate(idat.buf[2..idat.len]);
    defer if (raw_out.buf.len > 0) alloc.free(raw_out.buf);
    const raw = raw_out.buf[0..raw_out.len];
    const out = try alloc.alloc(u8, @as(usize, png.w) * png.h * 4);
    if (interlace == 0) {
        _ = try png.pass(raw, png.w, png.h, out, 0, 1, 0, 1);
    } else {
        const starts = [_][4]u32{ .{ 0, 0, 8, 8 }, .{ 4, 0, 8, 8 }, .{ 0, 4, 4, 8 }, .{ 2, 0, 4, 4 }, .{ 0, 2, 2, 4 }, .{ 1, 0, 2, 2 }, .{ 0, 1, 1, 2 } };
        var off: usize = 0;
        for (starts) |s| {
            const pw = (png.w + s[2] - 1 - s[0]) / s[2];
            const ph = (png.h + s[3] - 1 - s[1]) / s[3];
            if (s[0] >= png.w) continue;
            if (s[1] >= png.h) continue;
            off += try png.pass(raw[off..], pw, ph, out, s[1], s[3], s[0], s[2]);
        }
    }
    ow.* = png.w;
    oh.* = png.h;
    return out;
}

// ---------------------------------------------------------------- JPEG (baseline)
const ZIGZAG = [_]u8{ 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

const JHuff = struct {
    counts: [17]u8 = @splat(0),
    symbols: [256]u8 = @splat(0),
    maxcode: [18]i32 = @splat(-1),
    valptr: [17]i32 = @splat(0),
    mincode: [17]i32 = @splat(0),
    fn build(self: *JHuff) void {
        var code: i32 = 0;
        var k: i32 = 0;
        var l: usize = 1;
        while (l <= 16) : (l += 1) {
            self.valptr[l] = k;
            self.mincode[l] = code;
            code += self.counts[l];
            k += self.counts[l];
            self.maxcode[l] = if (self.counts[l] > 0) code - 1 else -1;
            code <<= 1;
        }
        self.maxcode[17] = 0x7fffffff;
    }
};

const JReader = struct {
    data: []const u8,
    pos: usize,
    buf: u32 = 0,
    cnt: u5 = 0,
    fn bit(self: *JReader) !u32 {
        if (self.cnt == 0) {
            if (self.pos >= self.data.len) return error.Truncated;
            var b = self.data[self.pos];
            self.pos += 1;
            if (b == 0xFF) {
                const n = if (self.pos < self.data.len) self.data[self.pos] else 0;
                if (n == 0) {
                    self.pos += 1;
                } else if (n >= 0xD0 and n <= 0xD7) {
                    b = 0;
                } else {
                    b = 0;
                }
            }
            self.buf = b;
            self.cnt = 8;
        }
        self.cnt -= 1;
        return (self.buf >> self.cnt) & 1;
    }
    fn bits(self: *JReader, n: u5) !i32 {
        var v: i32 = 0;
        var i: u5 = 0;
        while (i < n) : (i += 1) v = (v << 1) | @as(i32, @intCast(try self.bit()));
        return v;
    }
    fn decode(self: *JReader, h: *const JHuff) !u8 {
        var code: i32 = 0;
        var l: usize = 1;
        while (l <= 16) : (l += 1) {
            code = (code << 1) | @as(i32, @intCast(try self.bit()));
            if (code <= h.maxcode[l]) return h.symbols[@intCast(h.valptr[l] + code - h.mincode[l])];
        }
        return error.BadCode;
    }
    fn reset(self: *JReader) void {
        self.cnt = 0;
        // skip to after an RSTn marker
        while (self.pos + 1 < self.data.len) {
            if (self.data[self.pos] == 0xFF and self.data[self.pos + 1] >= 0xD0 and self.data[self.pos + 1] <= 0xD7) {
                self.pos += 2;
                return;
            }
            self.pos += 1;
        }
    }
};

fn extend(v: i32, t: u5) i32 {
    if (t == 0) return 0;
    const vt: i32 = @as(i32, 1) << @intCast(t - 1);
    if (v < vt) return v - (@as(i32, 1) << @intCast(t)) + 1;
    return v;
}

fn idct(in: *const [64]i32, out: *[64]u8) void {
    var tmp: [64]f32 = undefined;
    // separable float IDCT
    var u: usize = 0;
    while (u < 8) : (u += 1) {
        var x: usize = 0;
        while (x < 8) : (x += 1) {
            var s: f32 = 0;
            var v: usize = 0;
            while (v < 8) : (v += 1) {
                const cv: f32 = if (v == 0) 0.70710678 else 1.0;
                s += cv * @as(f32, @floatFromInt(in[u * 8 + v])) * @cos((2.0 * @as(f32, @floatFromInt(x)) + 1.0) * @as(f32, @floatFromInt(v)) * std.math.pi / 16.0);
            }
            tmp[u * 8 + x] = s / 2.0;
        }
    }
    var y: usize = 0;
    while (y < 8) : (y += 1) {
        var x: usize = 0;
        while (x < 8) : (x += 1) {
            var s: f32 = 0;
            var uu: usize = 0;
            while (uu < 8) : (uu += 1) {
                const cu: f32 = if (uu == 0) 0.70710678 else 1.0;
                s += cu * tmp[uu * 8 + x] * @cos((2.0 * @as(f32, @floatFromInt(y)) + 1.0) * @as(f32, @floatFromInt(uu)) * std.math.pi / 16.0);
            }
            const val = @round(s / 2.0 + 128.0);
            out[y * 8 + x] = @intFromFloat(@max(0.0, @min(255.0, val)));
        }
    }
}

const Comp = struct { id: u8, h: u8, v: u8, tq: u8, td: u8 = 0, ta: u8 = 0, pred: i32 = 0, plane: []u8 = &[_]u8{}, pw: usize = 0, ph: usize = 0 };

fn decode_jpeg(data: []const u8, ow: *u32, oh: *u32) ![]u8 {
    if (data.len < 4 or data[0] != 0xFF or data[1] != 0xD8) return error.NotJpeg;
    var qt: [4][64]u16 = undefined;
    var dc: [4]JHuff = @splat(.{});
    var ac: [4]JHuff = @splat(.{});
    var comps: [4]Comp = undefined;
    var ncomp: usize = 0;
    var w: usize = 0;
    var h: usize = 0;
    var restart: usize = 0;
    var p: usize = 2;
    var hmax: u8 = 1;
    var vmax: u8 = 1;
    while (p + 4 <= data.len) {
        if (data[p] != 0xFF) {
            p += 1;
            continue;
        }
        const m = data[p + 1];
        if (m == 0xD8 or m == 0x01 or (m >= 0xD0 and m <= 0xD7) or m == 0xFF) {
            p += if (m == 0xFF) 1 else 2;
            continue;
        }
        if (m == 0xD9) break;
        const len: usize = (@as(usize, data[p + 2]) << 8) | data[p + 3];
        if (p + 2 + len > data.len) return error.Truncated;
        const seg = data[p + 4 .. p + 2 + len];
        switch (m) {
            0xDB => {
                var i: usize = 0;
                while (i < seg.len) {
                    const pq = seg[i] >> 4;
                    const tq = seg[i] & 15;
                    i += 1;
                    for (0..64) |k| {
                        if (pq == 0) {
                            qt[tq & 3][ZIGZAG[k]] = seg[i];
                            i += 1;
                        } else {
                            qt[tq & 3][ZIGZAG[k]] = (@as(u16, seg[i]) << 8) | seg[i + 1];
                            i += 2;
                        }
                    }
                }
            },
            0xC0, 0xC1 => {
                h = (@as(usize, seg[1]) << 8) | seg[2];
                w = (@as(usize, seg[3]) << 8) | seg[4];
                ncomp = seg[5];
                if (ncomp > 4 or ncomp == 0) return error.Unsupported;
                for (0..ncomp) |c| {
                    comps[c] = Comp{ .id = seg[6 + 3 * c], .h = seg[7 + 3 * c] >> 4, .v = seg[7 + 3 * c] & 15, .tq = seg[8 + 3 * c] & 3 };
                    hmax = @max(hmax, comps[c].h);
                    vmax = @max(vmax, comps[c].v);
                }
            },
            0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF => return error.Unsupported,
            0xC4 => {
                var i: usize = 0;
                while (i < seg.len) {
                    const tc = seg[i] >> 4;
                    const th = seg[i] & 3;
                    i += 1;
                    var t = JHuff{};
                    var total: usize = 0;
                    for (1..17) |l| {
                        t.counts[l] = seg[i + l - 1];
                        total += seg[i + l - 1];
                    }
                    i += 16;
                    for (0..total) |k| t.symbols[k] = seg[i + k];
                    i += total;
                    t.build();
                    if (tc == 0) dc[th] = t else ac[th] = t;
                }
            },
            0xDD => restart = (@as(usize, seg[0]) << 8) | seg[1],
            0xDA => {
                const ns = seg[0];
                for (0..ns) |i| {
                    const cid = seg[1 + 2 * i];
                    for (0..ncomp) |c| if (comps[c].id == cid) {
                        comps[c].td = seg[2 + 2 * i] >> 4;
                        comps[c].ta = seg[2 + 2 * i] & 15;
                    };
                }
                if (w == 0 or h == 0) return error.NoFrame;
                // allocate planes (padded to whole MCUs)
                const mcux = (w + 8 * @as(usize, hmax) - 1) / (8 * @as(usize, hmax));
                const mcuy = (h + 8 * @as(usize, vmax) - 1) / (8 * @as(usize, vmax));
                for (0..ncomp) |c| {
                    comps[c].pw = mcux * comps[c].h * 8;
                    comps[c].ph = mcuy * comps[c].v * 8;
                    comps[c].plane = try alloc.alloc(u8, comps[c].pw * comps[c].ph);
                    comps[c].pred = 0;
                }
                var r = JReader{ .data = data, .pos = p + 2 + len };
                var mcu: usize = 0;
                const total = mcux * mcuy;
                while (mcu < total) : (mcu += 1) {
                    if (restart > 0 and mcu > 0 and mcu % restart == 0) {
                        r.reset();
                        for (0..ncomp) |c| comps[c].pred = 0;
                    }
                    const mx = mcu % mcux;
                    const my = mcu / mcux;
                    for (0..ncomp) |c| {
                        var by: usize = 0;
                        while (by < comps[c].v) : (by += 1) {
                            var bx: usize = 0;
                            while (bx < comps[c].h) : (bx += 1) {
                                var blk: [64]i32 = @splat(0);
                                const t = try r.decode(&dc[comps[c].td]);
                                const diff = extend(try r.bits(@intCast(t)), @intCast(t));
                                comps[c].pred += diff;
                                blk[0] = comps[c].pred * @as(i32, qt[comps[c].tq][0]);
                                var k: usize = 1;
                                while (k < 64) {
                                    const rs = try r.decode(&ac[comps[c].ta]);
                                    const run = rs >> 4;
                                    const sz = rs & 15;
                                    if (sz == 0) {
                                        if (run == 15) {
                                            k += 16;
                                            continue;
                                        }
                                        break;
                                    }
                                    k += run;
                                    if (k > 63) break;
                                    const v = extend(try r.bits(@intCast(sz)), @intCast(sz));
                                    blk[ZIGZAG[k]] = v * @as(i32, qt[comps[c].tq][ZIGZAG[k]]);
                                    k += 1;
                                }
                                var pix: [64]u8 = undefined;
                                idct(&blk, &pix);
                                const ox = (mx * comps[c].h + bx) * 8;
                                const oy = (my * comps[c].v + by) * 8;
                                for (0..8) |yy| for (0..8) |xx| {
                                    comps[c].plane[(oy + yy) * comps[c].pw + ox + xx] = pix[yy * 8 + xx];
                                };
                            }
                        }
                    }
                }
                // color conversion
                const out = try alloc.alloc(u8, w * h * 4);
                for (0..h) |y| for (0..w) |x| {
                    const o = (y * w + x) * 4;
                    var vals: [4]f32 = undefined;
                    for (0..ncomp) |c| {
                        const sx = x * comps[c].h / hmax;
                        const sy = y * comps[c].v / vmax;
                        vals[c] = @floatFromInt(comps[c].plane[sy * comps[c].pw + sx]);
                    }
                    if (ncomp >= 3) {
                        const yy = vals[0];
                        const cb = vals[1] - 128.0;
                        const cr = vals[2] - 128.0;
                        out[o] = @intFromFloat(@max(0.0, @min(255.0, @round(yy + 1.402 * cr))));
                        out[o + 1] = @intFromFloat(@max(0.0, @min(255.0, @round(yy - 0.344136 * cb - 0.714136 * cr))));
                        out[o + 2] = @intFromFloat(@max(0.0, @min(255.0, @round(yy + 1.772 * cb))));
                    } else {
                        const v: u8 = @intFromFloat(vals[0]);
                        out[o] = v;
                        out[o + 1] = v;
                        out[o + 2] = v;
                    }
                    out[o + 3] = 255;
                };
                for (0..ncomp) |c| alloc.free(comps[c].plane);
                ow.* = @intCast(w);
                oh.* = @intCast(h);
                return out;
            },
            else => {},
        }
        p += 2 + len;
    }
    return error.NoScan;
}

// ---------------------------------------------------------------- C ABI
export fn kd_decode(data: [*]const u8, len: usize, w: *u32, h: *u32) ?[*]u8 {
    const d = data[0..len];
    const r = if (len >= 2 and d[0] == 0xFF and d[1] == 0xD8) decode_jpeg(d, w, h) else decode_png(d, w, h);
    const out = r catch return null;
    return out.ptr;
}

export fn kd_free(p: [*]u8, len: usize) void {
    alloc.free(p[0..len]);
}

test "inflate a stored block" {
    const z = [_]u8{ 0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c' };
    const out = try inflate(&z);
    try std.testing.expectEqualSlices(u8, "abc", out.buf[0..out.len]);
}

test "decode a PNG: an 8x8 checker tile and an alpha dot" {
    var w: u32 = 0;
    var h: u32 = 0;
    const px = try decode_png(@embedFile("testdata/checker.png"), &w, &h);
    defer alloc.free(px);
    try std.testing.expectEqual(@as(u32, 16), w);
    try std.testing.expectEqualSlices(u8, &.{ 240, 240, 240, 255 }, px[0..4]);
    const o = (0 * 16 + 8) * 4; // the tile to the right is red
    try std.testing.expectEqualSlices(u8, &.{ 200, 60, 60, 255 }, px[o .. o + 4]);
    const dot = try decode_png(@embedFile("testdata/dot.png"), &w, &h);
    defer alloc.free(dot);
    try std.testing.expectEqual(@as(u8, 0), dot[3]); // a transparent corner
    try std.testing.expectEqual(@as(u8, 255), dot[(10 * 20 + 10) * 4 + 3]);
}
