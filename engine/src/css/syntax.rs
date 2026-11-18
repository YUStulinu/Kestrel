//! CSS Syntax Level 3: the tokenizer and the parser into rules and declarations.

#[derive(Clone, Debug, PartialEq)]
pub enum Tok {
    Ident(String),
    Function(String),
    AtKeyword(String),
    Hash(String, bool), // value, is an identifier
    Str(String),
    BadStr,
    Url(String),
    BadUrl,
    Delim(char),
    Number(f64, bool), // value, is an integer
    Percentage(f64),
    Dimension(f64, String),
    Ws,
    Cdo,
    Cdc,
    Colon,
    Semicolon,
    Comma,
    LBracket,
    RBracket,
    LParen,
    RParen,
    LBrace,
    RBrace,
    Eof,
}

pub fn tokenize(src: &str) -> Vec<Tok> {
    let s: Vec<char> = preprocess(src);
    let mut t = Tokenizer { s, i: 0 };
    let mut out = vec![];
    loop {
        let x = t.next();
        if x == Tok::Eof {
            break;
        }
        out.push(x);
    }
    out
}

fn preprocess(src: &str) -> Vec<char> {
    let mut v = Vec::with_capacity(src.len());
    let mut it = src.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            '\r' => {
                if it.peek() == Some(&'\n') {
                    it.next();
                }
                v.push('\n');
            }
            '\x0C' => v.push('\n'),
            '\0' => v.push('\u{FFFD}'),
            c => v.push(c),
        }
    }
    v
}

struct Tokenizer {
    s: Vec<char>,
    i: usize,
}

fn is_name_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_' || c as u32 >= 0x80
}
fn is_name(c: char) -> bool {
    is_name_start(c) || c.is_ascii_digit() || c == '-'
}
fn is_ws(c: char) -> bool {
    c == ' ' || c == '\t' || c == '\n'
}

impl Tokenizer {
    fn peek(&self, k: usize) -> char {
        self.s.get(self.i + k).copied().unwrap_or('\0')
    }
    fn eof_at(&self, k: usize) -> bool {
        self.i + k >= self.s.len()
    }
    fn valid_escape(&self, k: usize) -> bool {
        self.peek(k) == '\\' && !self.eof_at(k) && self.peek(k + 1) != '\n' && !(self.eof_at(k + 1) && false)
    }
    fn starts_ident(&self, k: usize) -> bool {
        let c = self.peek(k);
        if self.eof_at(k) {
            return false;
        }
        if c == '-' {
            let d = self.peek(k + 1);
            return (!self.eof_at(k + 1) && (is_name_start(d) || d == '-')) || self.valid_escape(k + 1);
        }
        if is_name_start(c) {
            return true;
        }
        c == '\\' && self.valid_escape(k)
    }
    fn starts_number(&self) -> bool {
        let c = self.peek(0);
        if c == '+' || c == '-' {
            let d = self.peek(1);
            return d.is_ascii_digit() || (d == '.' && self.peek(2).is_ascii_digit());
        }
        if c == '.' {
            return self.peek(1).is_ascii_digit();
        }
        c.is_ascii_digit()
    }
    fn escape(&mut self) -> char {
        // after the backslash
        if self.eof_at(0) {
            return '\u{FFFD}';
        }
        let c = self.s[self.i];
        self.i += 1;
        if c.is_ascii_hexdigit() {
            let mut v = c.to_digit(16).unwrap();
            let mut n = 1;
            while n < 6 && !self.eof_at(0) && self.peek(0).is_ascii_hexdigit() {
                v = v * 16 + self.peek(0).to_digit(16).unwrap();
                self.i += 1;
                n += 1;
            }
            if !self.eof_at(0) && is_ws(self.peek(0)) {
                self.i += 1;
            }
            if v == 0 || (0xD800..=0xDFFF).contains(&v) || v > 0x10FFFF {
                return '\u{FFFD}';
            }
            return char::from_u32(v).unwrap_or('\u{FFFD}');
        }
        c
    }
    fn name(&mut self) -> String {
        let mut out = String::new();
        loop {
            if self.eof_at(0) {
                break;
            }
            let c = self.peek(0);
            if is_name(c) {
                out.push(c);
                self.i += 1;
            } else if self.valid_escape(0) {
                self.i += 1;
                out.push(self.escape());
            } else {
                break;
            }
        }
        out
    }
    fn number(&mut self) -> (f64, bool) {
        let mut repr = String::new();
        let mut int = true;
        if self.peek(0) == '+' || self.peek(0) == '-' {
            repr.push(self.peek(0));
            self.i += 1;
        }
        while self.peek(0).is_ascii_digit() && !self.eof_at(0) {
            repr.push(self.peek(0));
            self.i += 1;
        }
        if self.peek(0) == '.' && self.peek(1).is_ascii_digit() {
            int = false;
            repr.push('.');
            self.i += 1;
            while self.peek(0).is_ascii_digit() && !self.eof_at(0) {
                repr.push(self.peek(0));
                self.i += 1;
            }
        }
        let e = self.peek(0);
        if (e == 'e' || e == 'E')
            && (self.peek(1).is_ascii_digit() || ((self.peek(1) == '+' || self.peek(1) == '-') && self.peek(2).is_ascii_digit()))
        {
            int = false;
            repr.push('e');
            self.i += 1;
            if self.peek(0) == '+' || self.peek(0) == '-' {
                repr.push(self.peek(0));
                self.i += 1;
            }
            while self.peek(0).is_ascii_digit() && !self.eof_at(0) {
                repr.push(self.peek(0));
                self.i += 1;
            }
        }
        (repr.parse().unwrap_or(0.0), int)
    }
    fn numeric(&mut self) -> Tok {
        let (v, int) = self.number();
        if self.starts_ident(0) {
            let u = self.name();
            return Tok::Dimension(v, u.to_ascii_lowercase());
        }
        if self.peek(0) == '%' && !self.eof_at(0) {
            self.i += 1;
            return Tok::Percentage(v);
        }
        Tok::Number(v, int)
    }
    fn string(&mut self, q: char) -> Tok {
        self.i += 1;
        let mut out = String::new();
        loop {
            if self.eof_at(0) {
                return Tok::Str(out);
            }
            let c = self.peek(0);
            self.i += 1;
            if c == q {
                return Tok::Str(out);
            }
            if c == '\n' {
                self.i -= 1;
                return Tok::BadStr;
            }
            if c == '\\' {
                if self.eof_at(0) {
                    continue;
                }
                if self.peek(0) == '\n' {
                    self.i += 1;
                    continue;
                }
                out.push(self.escape());
                continue;
            }
            out.push(c);
        }
    }
    fn url(&mut self) -> Tok {
        while is_ws(self.peek(0)) && !self.eof_at(0) {
            self.i += 1;
        }
        let mut out = String::new();
        loop {
            if self.eof_at(0) {
                return Tok::Url(out);
            }
            let c = self.peek(0);
            self.i += 1;
            match c {
                ')' => return Tok::Url(out),
                c if is_ws(c) => {
                    while is_ws(self.peek(0)) && !self.eof_at(0) {
                        self.i += 1;
                    }
                    if self.peek(0) == ')' || self.eof_at(0) {
                        self.i += 1;
                        return Tok::Url(out);
                    }
                    self.bad_url_rest();
                    return Tok::BadUrl;
                }
                '"' | '\'' | '(' => {
                    self.bad_url_rest();
                    return Tok::BadUrl;
                }
                '\\' => {
                    if self.valid_escape(0) || !self.eof_at(0) {
                        out.push(self.escape());
                    } else {
                        self.bad_url_rest();
                        return Tok::BadUrl;
                    }
                }
                c => out.push(c),
            }
        }
    }
    fn bad_url_rest(&mut self) {
        while !self.eof_at(0) {
            let c = self.peek(0);
            self.i += 1;
            if c == ')' {
                return;
            }
            if c == '\\' && !self.eof_at(0) {
                self.i += 1;
            }
        }
    }
    fn ident_like(&mut self) -> Tok {
        let n = self.name();
        if n.eq_ignore_ascii_case("url") && self.peek(0) == '(' {
            self.i += 1;
            let mut k = 0;
            while is_ws(self.peek(k)) {
                k += 1;
            }
            let c = self.peek(k);
            if c == '"' || c == '\'' {
                self.i += k.saturating_sub(1);
                return Tok::Function(n);
            }
            return self.url();
        }
        if self.peek(0) == '(' {
            self.i += 1;
            return Tok::Function(n);
        }
        Tok::Ident(n)
    }
    fn next(&mut self) -> Tok {
        // comments
        while self.peek(0) == '/' && self.peek(1) == '*' {
            self.i += 2;
            while !self.eof_at(0) && !(self.peek(0) == '*' && self.peek(1) == '/') {
                self.i += 1;
            }
            self.i = (self.i + 2).min(self.s.len());
        }
        if self.eof_at(0) {
            return Tok::Eof;
        }
        let c = self.peek(0);
        if is_ws(c) {
            while !self.eof_at(0) && is_ws(self.peek(0)) {
                self.i += 1;
            }
            return Tok::Ws;
        }
        match c {
            '"' | '\'' => self.string(c),
            '#' => {
                if is_name(self.peek(1)) || self.valid_escape(1) {
                    self.i += 1;
                    let is_id = self.starts_ident(0);
                    Tok::Hash(self.name(), is_id)
                } else {
                    self.i += 1;
                    Tok::Delim('#')
                }
            }
            '(' => {
                self.i += 1;
                Tok::LParen
            }
            ')' => {
                self.i += 1;
                Tok::RParen
            }
            '+' | '.' => {
                if self.starts_number() {
                    self.numeric()
                } else {
                    self.i += 1;
                    Tok::Delim(c)
                }
            }
            ',' => {
                self.i += 1;
                Tok::Comma
            }
            '-' => {
                if self.starts_number() {
                    self.numeric()
                } else if self.peek(1) == '-' && self.peek(2) == '>' {
                    self.i += 3;
                    Tok::Cdc
                } else if self.starts_ident(0) {
                    self.ident_like()
                } else {
                    self.i += 1;
                    Tok::Delim('-')
                }
            }
            ':' => {
                self.i += 1;
                Tok::Colon
            }
            ';' => {
                self.i += 1;
                Tok::Semicolon
            }
            '<' => {
                if self.peek(1) == '!' && self.peek(2) == '-' && self.peek(3) == '-' {
                    self.i += 4;
                    Tok::Cdo
                } else {
                    self.i += 1;
                    Tok::Delim('<')
                }
            }
            '@' => {
                if self.starts_ident(1) {
                    self.i += 1;
                    Tok::AtKeyword(self.name())
                } else {
                    self.i += 1;
                    Tok::Delim('@')
                }
            }
            '[' => {
                self.i += 1;
                Tok::LBracket
            }
            ']' => {
                self.i += 1;
                Tok::RBracket
            }
            '{' => {
                self.i += 1;
                Tok::LBrace
            }
            '}' => {
                self.i += 1;
                Tok::RBrace
            }
            '\\' => {
                if self.valid_escape(0) {
                    self.ident_like()
                } else {
                    self.i += 1;
                    Tok::Delim('\\')
                }
            }
            c if c.is_ascii_digit() => self.numeric(),
            c if is_name_start(c) => self.ident_like(),
            c => {
                self.i += 1;
                Tok::Delim(c)
            }
        }
    }
}

// ------------------------------------------------------------------ rules
#[derive(Clone, Debug)]
pub struct Declaration {
    pub name: String,
    pub value: Vec<Tok>,
    pub important: bool,
}

#[derive(Clone, Debug)]
pub enum Rule {
    Style { prelude: Vec<Tok>, decls: Vec<Declaration> },
    At { name: String, prelude: Vec<Tok>, block: Option<Vec<Tok>> },
}

pub struct Parser<'a> {
    t: &'a [Tok],
    i: usize,
}

impl<'a> Parser<'a> {
    pub fn new(t: &'a [Tok]) -> Self {
        Parser { t, i: 0 }
    }
    fn peek(&self) -> &Tok {
        self.t.get(self.i).unwrap_or(&Tok::Eof)
    }
    fn next(&mut self) -> Tok {
        let x = self.peek().clone();
        self.i += 1;
        x
    }
    /// a component value, with nested blocks flattened into the output
    fn component(&mut self, out: &mut Vec<Tok>) {
        let t = self.next();
        let close = match &t {
            Tok::LBrace => Some(Tok::RBrace),
            Tok::LBracket => Some(Tok::RBracket),
            Tok::LParen | Tok::Function(_) => Some(Tok::RParen),
            _ => None,
        };
        out.push(t);
        if let Some(close) = close {
            loop {
                if *self.peek() == Tok::Eof {
                    out.push(close);
                    return;
                }
                if *self.peek() == close {
                    out.push(self.next());
                    return;
                }
                self.component(out);
            }
        }
    }
    pub fn rules(&mut self, top: bool) -> Vec<Rule> {
        let mut out = vec![];
        loop {
            match self.peek() {
                Tok::Eof => return out,
                Tok::Ws => {
                    self.i += 1;
                }
                Tok::Cdo | Tok::Cdc if top => {
                    self.i += 1;
                }
                Tok::AtKeyword(_) => {
                    if let Some(r) = self.at_rule() {
                        out.push(r);
                    }
                }
                _ => {
                    if let Some(r) = self.qualified_rule() {
                        out.push(r);
                    }
                }
            }
        }
    }
    fn at_rule(&mut self) -> Option<Rule> {
        let Tok::AtKeyword(name) = self.next() else { return None };
        let mut prelude = vec![];
        loop {
            match self.peek() {
                Tok::Semicolon => {
                    self.i += 1;
                    return Some(Rule::At { name: name.to_ascii_lowercase(), prelude, block: None });
                }
                Tok::Eof => return Some(Rule::At { name: name.to_ascii_lowercase(), prelude, block: None }),
                Tok::LBrace => {
                    let mut b = vec![];
                    self.component(&mut b);
                    // strip the braces
                    b.remove(0);
                    b.pop();
                    return Some(Rule::At { name: name.to_ascii_lowercase(), prelude, block: Some(b) });
                }
                _ => self.component(&mut prelude),
            }
        }
    }
    fn qualified_rule(&mut self) -> Option<Rule> {
        let mut prelude = vec![];
        loop {
            match self.peek() {
                Tok::Eof => return None,
                Tok::LBrace => {
                    let mut b = vec![];
                    self.component(&mut b);
                    b.remove(0);
                    b.pop();
                    return Some(Rule::Style { prelude, decls: declarations(&b) });
                }
                _ => self.component(&mut prelude),
            }
        }
    }
}

/// a declaration list (a style block's contents, or a style attribute)
pub fn declarations(t: &[Tok]) -> Vec<Declaration> {
    let mut out = vec![];
    let mut p = Parser::new(t);
    loop {
        match p.peek().clone() {
            Tok::Eof => return out,
            Tok::Ws | Tok::Semicolon => p.i += 1,
            Tok::AtKeyword(_) => {
                p.at_rule();
            }
            Tok::Ident(name) => {
                p.i += 1;
                let mut value = vec![];
                while !matches!(p.peek(), Tok::Semicolon | Tok::Eof) {
                    p.component(&mut value);
                }
                // name ws* : value
                let mut v: &[Tok] = &value;
                while let Some(Tok::Ws) = v.first() {
                    v = &v[1..];
                }
                if let Some(Tok::Colon) = v.first() {
                    let mut vals: Vec<Tok> = v[1..].to_vec();
                    // !important
                    let mut important = false;
                    let trimmed: Vec<usize> = vals.iter().enumerate().filter(|(_, t)| **t != Tok::Ws).map(|(i, _)| i).collect();
                    if trimmed.len() >= 2 {
                        let a = trimmed[trimmed.len() - 2];
                        let b = trimmed[trimmed.len() - 1];
                        if vals[a] == Tok::Delim('!') && matches!(&vals[b], Tok::Ident(s) if s.eq_ignore_ascii_case("important")) {
                            important = true;
                            vals.truncate(a);
                        }
                    }
                    while let Some(Tok::Ws) = vals.last() {
                        vals.pop();
                    }
                    while let Some(Tok::Ws) = vals.first() {
                        vals.remove(0);
                    }
                    out.push(Declaration { name: name.to_ascii_lowercase(), value: vals, important });
                }
            }
            _ => {
                // skip to the next semicolon
                while !matches!(p.peek(), Tok::Semicolon | Tok::Eof) {
                    let mut x = vec![];
                    p.component(&mut x);
                }
            }
        }
    }
}

pub fn parse_stylesheet(src: &str) -> Vec<Rule> {
    let t = tokenize(src);
    Parser::new(&t).rules(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rules_and_decls() {
        let r = parse_stylesheet("p, div > a { color: red !important; margin: 1px 2em } @media screen { b { x: y } }");
        assert_eq!(r.len(), 2);
        let Rule::Style { decls, .. } = &r[0] else { panic!() };
        assert_eq!(decls[0].name, "color");
        assert!(decls[0].important);
        assert_eq!(decls[1].value, vec![Tok::Dimension(1.0, "px".into()), Tok::Ws, Tok::Dimension(2.0, "em".into())]);
    }
}
