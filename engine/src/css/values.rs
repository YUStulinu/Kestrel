//! CSS values: lengths (with calc), colors.
use super::syntax::Tok;

/// a length: px + pct% of a reference (the containing block, usually)
#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Len {
    pub px: f32,
    pub pct: f32,
}

impl Len {
    pub const ZERO: Len = Len { px: 0.0, pct: 0.0 };
    pub fn px(v: f32) -> Len {
        Len { px: v, pct: 0.0 }
    }
    pub fn resolve(&self, reference: f32) -> f32 {
        self.px + self.pct / 100.0 * reference
    }
    pub fn has_pct(&self) -> bool {
        self.pct != 0.0
    }
}

/// the context for converting relative units
#[derive(Clone, Copy)]
pub struct Units {
    pub em: f32,
    pub rem: f32,
    pub vw: f32,
    pub vh: f32,
    pub ex: f32,
    pub ch: f32,
}

pub fn unit_px(v: f32, unit: &str, u: &Units) -> Option<f32> {
    Some(match unit {
        "px" => v,
        "em" => v * u.em,
        "rem" => v * u.rem,
        "ex" => v * u.ex,
        "ch" => v * u.ch,
        "pt" => v * 96.0 / 72.0,
        "pc" => v * 16.0,
        "in" => v * 96.0,
        "cm" => v * 96.0 / 2.54,
        "mm" => v * 96.0 / 25.4,
        "q" => v * 96.0 / 101.6,
        "vw" => v * u.vw / 100.0,
        "vh" => v * u.vh / 100.0,
        "vmin" => v * u.vw.min(u.vh) / 100.0,
        "vmax" => v * u.vw.max(u.vh) / 100.0,
        _ => return None,
    })
}

/// a single length-percentage token (or a calc() starting at `t[*i]`)
pub fn parse_len(t: &[Tok], i: &mut usize, u: &Units, allow_unitless_zero: bool) -> Option<Len> {
    match t.get(*i)? {
        Tok::Dimension(v, unit) => {
            let px = unit_px(*v as f32, unit, u)?;
            *i += 1;
            Some(Len::px(px))
        }
        Tok::Percentage(p) => {
            *i += 1;
            Some(Len { px: 0.0, pct: *p as f32 })
        }
        Tok::Number(v, _) if *v == 0.0 && allow_unitless_zero => {
            *i += 1;
            Some(Len::ZERO)
        }
        Tok::Function(f) if f.eq_ignore_ascii_case("calc") || f.eq_ignore_ascii_case("-webkit-calc") => {
            // the arguments up to the matching parenthesis
            let start = *i + 1;
            let mut depth = 1;
            let mut j = start;
            while j < t.len() {
                match &t[j] {
                    Tok::Function(_) | Tok::LParen => depth += 1,
                    Tok::RParen => {
                        depth -= 1;
                        if depth == 0 {
                            break;
                        }
                    }
                    _ => {}
                }
                j += 1;
            }
            let args: Vec<Tok> = t[start..j].iter().filter(|x| **x != Tok::Ws).cloned().collect();
            *i = j + 1;
            let mut k = 0;
            let v = calc_sum(&args, &mut k, u)?;
            if k != args.len() {
                return None;
            }
            match v {
                CalcVal::Len(l) => Some(l),
                CalcVal::Num(n) if n == 0.0 => Some(Len::ZERO),
                _ => None,
            }
        }
        _ => None,
    }
}

#[derive(Clone, Copy)]
enum CalcVal {
    Num(f32),
    Len(Len),
}

fn calc_sum(t: &[Tok], i: &mut usize, u: &Units) -> Option<CalcVal> {
    let mut acc = calc_product(t, i, u)?;
    loop {
        let sign = match t.get(*i) {
            Some(Tok::Delim('+')) => 1.0,
            Some(Tok::Delim('-')) => -1.0,
            // "+5px" may tokenize as a signed dimension
            Some(Tok::Dimension(v, _)) | Some(Tok::Number(v, _)) if *v < 0.0 => 0.0,
            _ => return Some(acc),
        };
        if sign != 0.0 {
            *i += 1;
        }
        let rhs = calc_product(t, i, u)?;
        let s = if sign == 0.0 { 1.0 } else { sign };
        acc = match (acc, rhs) {
            (CalcVal::Num(a), CalcVal::Num(b)) => CalcVal::Num(a + s * b),
            (CalcVal::Len(a), CalcVal::Len(b)) => CalcVal::Len(Len { px: a.px + s * b.px, pct: a.pct + s * b.pct }),
            _ => return None,
        };
    }
}
fn calc_product(t: &[Tok], i: &mut usize, u: &Units) -> Option<CalcVal> {
    let mut acc = calc_value(t, i, u)?;
    loop {
        let op = match t.get(*i) {
            Some(Tok::Delim('*')) => '*',
            Some(Tok::Delim('/')) => '/',
            _ => return Some(acc),
        };
        *i += 1;
        let rhs = calc_value(t, i, u)?;
        acc = match (op, acc, rhs) {
            ('*', CalcVal::Num(a), CalcVal::Num(b)) => CalcVal::Num(a * b),
            ('*', CalcVal::Len(a), CalcVal::Num(b)) | ('*', CalcVal::Num(b), CalcVal::Len(a)) => {
                CalcVal::Len(Len { px: a.px * b, pct: a.pct * b })
            }
            ('/', CalcVal::Num(a), CalcVal::Num(b)) if b != 0.0 => CalcVal::Num(a / b),
            ('/', CalcVal::Len(a), CalcVal::Num(b)) if b != 0.0 => CalcVal::Len(Len { px: a.px / b, pct: a.pct / b }),
            _ => return None,
        };
    }
}
fn calc_value(t: &[Tok], i: &mut usize, u: &Units) -> Option<CalcVal> {
    match t.get(*i)? {
        Tok::Number(v, _) => {
            *i += 1;
            Some(CalcVal::Num(*v as f32))
        }
        Tok::LParen | Tok::Function(_) => {
            let is_fn = matches!(t.get(*i)?, Tok::Function(_));
            if is_fn {
                let name = if let Tok::Function(f) = &t[*i] { f.to_ascii_lowercase() } else { String::new() };
                if name != "calc" {
                    return None;
                }
            }
            *i += 1;
            let v = calc_sum(t, i, u)?;
            if t.get(*i) != Some(&Tok::RParen) {
                return None;
            }
            *i += 1;
            Some(v)
        }
        _ => {
            let l = parse_len(t, i, u, false)?;
            Some(CalcVal::Len(l))
        }
    }
}

// ------------------------------------------------------------------ colors
#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Color {
    pub const TRANSPARENT: Color = Color { r: 0, g: 0, b: 0, a: 0 };
    pub const BLACK: Color = Color { r: 0, g: 0, b: 0, a: 255 };
    pub fn rgb(r: u8, g: u8, b: u8) -> Color {
        Color { r, g, b, a: 255 }
    }
}

const NAMED: &[(&str, u32)] = &[
    ("aliceblue", 0xf0f8ff), ("antiquewhite", 0xfaebd7), ("aqua", 0x00ffff), ("aquamarine", 0x7fffd4), ("azure", 0xf0ffff),
    ("beige", 0xf5f5dc), ("bisque", 0xffe4c4), ("black", 0x000000), ("blanchedalmond", 0xffebcd), ("blue", 0x0000ff),
    ("blueviolet", 0x8a2be2), ("brown", 0xa52a2a), ("burlywood", 0xdeb887), ("cadetblue", 0x5f9ea0), ("chartreuse", 0x7fff00),
    ("chocolate", 0xd2691e), ("coral", 0xff7f50), ("cornflowerblue", 0x6495ed), ("cornsilk", 0xfff8dc), ("crimson", 0xdc143c),
    ("cyan", 0x00ffff), ("darkblue", 0x00008b), ("darkcyan", 0x008b8b), ("darkgoldenrod", 0xb8860b), ("darkgray", 0xa9a9a9),
    ("darkgreen", 0x006400), ("darkgrey", 0xa9a9a9), ("darkkhaki", 0xbdb76b), ("darkmagenta", 0x8b008b),
    ("darkolivegreen", 0x556b2f), ("darkorange", 0xff8c00), ("darkorchid", 0x9932cc), ("darkred", 0x8b0000),
    ("darksalmon", 0xe9967a), ("darkseagreen", 0x8fbc8f), ("darkslateblue", 0x483d8b), ("darkslategray", 0x2f4f4f),
    ("darkslategrey", 0x2f4f4f), ("darkturquoise", 0x00ced1), ("darkviolet", 0x9400d3), ("deeppink", 0xff1493),
    ("deepskyblue", 0x00bfff), ("dimgray", 0x696969), ("dimgrey", 0x696969), ("dodgerblue", 0x1e90ff), ("firebrick", 0xb22222),
    ("floralwhite", 0xfffaf0), ("forestgreen", 0x228b22), ("fuchsia", 0xff00ff), ("gainsboro", 0xdcdcdc), ("ghostwhite", 0xf8f8ff),
    ("gold", 0xffd700), ("goldenrod", 0xdaa520), ("gray", 0x808080), ("green", 0x008000), ("greenyellow", 0xadff2f),
    ("grey", 0x808080), ("honeydew", 0xf0fff0), ("hotpink", 0xff69b4), ("indianred", 0xcd5c5c), ("indigo", 0x4b0082),
    ("ivory", 0xfffff0), ("khaki", 0xf0e68c), ("lavender", 0xe6e6fa), ("lavenderblush", 0xfff0f5), ("lawngreen", 0x7cfc00),
    ("lemonchiffon", 0xfffacd), ("lightblue", 0xadd8e6), ("lightcoral", 0xf08080), ("lightcyan", 0xe0ffff),
    ("lightgoldenrodyellow", 0xfafad2), ("lightgray", 0xd3d3d3), ("lightgreen", 0x90ee90), ("lightgrey", 0xd3d3d3),
    ("lightpink", 0xffb6c1), ("lightsalmon", 0xffa07a), ("lightseagreen", 0x20b2aa), ("lightskyblue", 0x87cefa),
    ("lightslategray", 0x778899), ("lightslategrey", 0x778899), ("lightsteelblue", 0xb0c4de), ("lightyellow", 0xffffe0),
    ("lime", 0x00ff00), ("limegreen", 0x32cd32), ("linen", 0xfaf0e6), ("magenta", 0xff00ff), ("maroon", 0x800000),
    ("mediumaquamarine", 0x66cdaa), ("mediumblue", 0x0000cd), ("mediumorchid", 0xba55d3), ("mediumpurple", 0x9370db),
    ("mediumseagreen", 0x3cb371), ("mediumslateblue", 0x7b68ee), ("mediumspringgreen", 0x00fa9a),
    ("mediumturquoise", 0x48d1cc), ("mediumvioletred", 0xc71585), ("midnightblue", 0x191970), ("mintcream", 0xf5fffa),
    ("mistyrose", 0xffe4e1), ("moccasin", 0xffe4b5), ("navajowhite", 0xffdead), ("navy", 0x000080), ("oldlace", 0xfdf5e6),
    ("olive", 0x808000), ("olivedrab", 0x6b8e23), ("orange", 0xffa500), ("orangered", 0xff4500), ("orchid", 0xda70d6),
    ("palegoldenrod", 0xeee8aa), ("palegreen", 0x98fb98), ("paleturquoise", 0xafeeee), ("palevioletred", 0xdb7093),
    ("papayawhip", 0xffefd5), ("peachpuff", 0xffdab9), ("peru", 0xcd853f), ("pink", 0xffc0cb), ("plum", 0xdda0dd),
    ("powderblue", 0xb0e0e6), ("purple", 0x800080), ("rebeccapurple", 0x663399), ("red", 0xff0000), ("rosybrown", 0xbc8f8f),
    ("royalblue", 0x4169e1), ("saddlebrown", 0x8b4513), ("salmon", 0xfa8072), ("sandybrown", 0xf4a460), ("seagreen", 0x2e8b57),
    ("seashell", 0xfff5ee), ("sienna", 0xa0522d), ("silver", 0xc0c0c0), ("skyblue", 0x87ceeb), ("slateblue", 0x6a5acd),
    ("slategray", 0x708090), ("slategrey", 0x708090), ("snow", 0xfffafa), ("springgreen", 0x00ff7f), ("steelblue", 0x4682b4),
    ("tan", 0xd2b48c), ("teal", 0x008080), ("thistle", 0xd8bfd8), ("tomato", 0xff6347), ("turquoise", 0x40e0d0),
    ("violet", 0xee82ee), ("wheat", 0xf5deb3), ("white", 0xffffff), ("whitesmoke", 0xf5f5f5), ("yellow", 0xffff00),
    ("yellowgreen", 0x9acd32),
];

/// None for currentcolor
pub enum ColorValue {
    Color(Color),
    Current,
}

fn hue_to_rgb(m1: f32, m2: f32, h: f32) -> f32 {
    let h = if h < 0.0 { h + 1.0 } else if h > 1.0 { h - 1.0 } else { h };
    if h * 6.0 < 1.0 {
        m1 + (m2 - m1) * h * 6.0
    } else if h * 2.0 < 1.0 {
        m2
    } else if h * 3.0 < 2.0 {
        m1 + (m2 - m1) * (2.0 / 3.0 - h) * 6.0
    } else {
        m1
    }
}

pub fn parse_color(t: &[Tok], i: &mut usize) -> Option<ColorValue> {
    let tok = t.get(*i)?.clone();
    match tok {
        Tok::Ident(n) => {
            let n = n.to_ascii_lowercase();
            *i += 1;
            if n == "transparent" {
                return Some(ColorValue::Color(Color::TRANSPARENT));
            }
            if n == "currentcolor" {
                return Some(ColorValue::Current);
            }
            // system colors (a few common ones)
            let sys = match n.as_str() {
                "canvas" | "window" | "buttonface" | "field" => Some(0xffffff),
                "canvastext" | "windowtext" | "buttontext" | "fieldtext" | "captiontext" => Some(0x000000),
                "linktext" => Some(0x0000ee),
                "graytext" => Some(0x6d6d6d),
                "highlight" => Some(0x3399ff),
                _ => None,
            };
            if let Some(v) = sys {
                return Some(ColorValue::Color(Color::rgb((v >> 16) as u8, (v >> 8) as u8, v as u8)));
            }
            let v = NAMED.iter().find(|(k, _)| *k == n)?.1;
            Some(ColorValue::Color(Color::rgb((v >> 16) as u8, (v >> 8) as u8, v as u8)))
        }
        Tok::Hash(h, _) => {
            *i += 1;
            let h: Vec<u8> = h.chars().map(|c| c.to_digit(16).map(|d| d as u8)).collect::<Option<Vec<_>>>()?;
            let c = match h.len() {
                3 => Color::rgb(h[0] * 17, h[1] * 17, h[2] * 17),
                4 => Color { r: h[0] * 17, g: h[1] * 17, b: h[2] * 17, a: h[3] * 17 },
                6 => Color::rgb(h[0] * 16 + h[1], h[2] * 16 + h[3], h[4] * 16 + h[5]),
                8 => Color { r: h[0] * 16 + h[1], g: h[2] * 16 + h[3], b: h[4] * 16 + h[5], a: h[6] * 16 + h[7] },
                _ => return None,
            };
            Some(ColorValue::Color(c))
        }
        Tok::Function(f) => {
            let f = f.to_ascii_lowercase();
            let mut j = *i + 1;
            let mut args: Vec<Tok> = vec![];
            while j < t.len() && t[j] != Tok::RParen {
                if t[j] != Tok::Ws {
                    args.push(t[j].clone());
                }
                j += 1;
            }
            *i = j + 1;
            let vals: Vec<&Tok> = args.iter().filter(|x| !matches!(x, Tok::Comma | Tok::Delim('/'))).collect();
            let num = |x: &Tok, scale: f32| -> Option<f32> {
                match x {
                    Tok::Number(v, _) => Some(*v as f32),
                    Tok::Percentage(p) => Some(*p as f32 * scale / 100.0),
                    Tok::Ident(n) if n.eq_ignore_ascii_case("none") => Some(0.0),
                    _ => None,
                }
            };
            let alpha = |x: Option<&&Tok>| -> Option<u8> {
                match x {
                    None => Some(255),
                    Some(t) => Some((num(t, 1.0)?.clamp(0.0, 1.0) * 255.0).round() as u8),
                }
            };
            match f.as_str() {
                "rgb" | "rgba" => {
                    if vals.len() < 3 {
                        return None;
                    }
                    let c = |k: usize| num(vals[k], 255.0).map(|v| v.clamp(0.0, 255.0).round() as u8);
                    Some(ColorValue::Color(Color { r: c(0)?, g: c(1)?, b: c(2)?, a: alpha(vals.get(3))? }))
                }
                "hsl" | "hsla" => {
                    if vals.len() < 3 {
                        return None;
                    }
                    let h = match vals[0] {
                        Tok::Number(v, _) => *v as f32,
                        Tok::Dimension(v, u) if u == "deg" => *v as f32,
                        Tok::Dimension(v, u) if u == "turn" => *v as f32 * 360.0,
                        Tok::Dimension(v, u) if u == "rad" => (*v as f32).to_degrees(),
                        _ => return None,
                    };
                    let s = num(vals[1], 1.0)?.clamp(0.0, 1.0);
                    let l = num(vals[2], 1.0)?.clamp(0.0, 1.0);
                    let h = ((h % 360.0) + 360.0) % 360.0 / 360.0;
                    let m2 = if l <= 0.5 { l * (s + 1.0) } else { l + s - l * s };
                    let m1 = l * 2.0 - m2;
                    let to = |x: f32| (x * 255.0).round().clamp(0.0, 255.0) as u8;
                    Some(ColorValue::Color(Color {
                        r: to(hue_to_rgb(m1, m2, h + 1.0 / 3.0)),
                        g: to(hue_to_rgb(m1, m2, h)),
                        b: to(hue_to_rgb(m1, m2, h - 1.0 / 3.0)),
                        a: alpha(vals.get(3))?,
                    }))
                }
                _ => None,
            }
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::syntax::tokenize;
    fn col(s: &str) -> Color {
        let t = tokenize(s);
        let mut i = 0;
        match parse_color(&t, &mut i).unwrap() {
            ColorValue::Color(c) => c,
            _ => panic!(),
        }
    }
    #[test]
    fn colors_and_calc() {
        assert_eq!(col("#0f0"), Color::rgb(0, 255, 0));
        assert_eq!(col("rgb(10%, 0, 255)"), Color::rgb(26, 0, 255));
        assert_eq!(col("hsl(120, 100%, 25%)"), Color::rgb(0, 128, 0));
        assert_eq!(col("navy"), Color::rgb(0, 0, 128));
        let u = Units { em: 10.0, rem: 16.0, vw: 800.0, vh: 600.0, ex: 5.0, ch: 5.0 };
        let t = tokenize("calc(50% - 2em + 4px)");
        let mut i = 0;
        assert_eq!(parse_len(&t, &mut i, &u, false), Some(Len { px: -16.0, pct: 50.0 }));
    }
}
