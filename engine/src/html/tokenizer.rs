//! The HTML tokenizer: the state machine of the HTML standard, section 13.2.5.
//! The tree builder drives it one token at a time and may switch its state
//! (RCDATA, RAWTEXT, script data, PLAINTEXT) and allow CDATA sections.
use super::entities;
use std::collections::VecDeque;

#[derive(Clone, Debug, PartialEq)]
pub struct Attr {
    pub name: String,
    pub value: String,
}

#[derive(Clone, Debug, PartialEq, Default)]
pub struct Tag {
    pub name: String,
    pub attrs: Vec<Attr>,
    pub self_closing: bool,
}

impl Tag {
    pub fn attr(&self, name: &str) -> Option<&str> {
        self.attrs.iter().find(|a| a.name == name).map(|a| a.value.as_str())
    }
}

#[derive(Clone, Debug, PartialEq, Default)]
pub struct Doctype {
    pub name: Option<String>,
    pub public_id: Option<String>,
    pub system_id: Option<String>,
    pub force_quirks: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Token {
    Doctype(Doctype),
    StartTag(Tag),
    EndTag(Tag),
    Comment(String),
    /// a processing instruction: target and data
    Pi(String, String),
    Char(char),
    Eof,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Data,
    Rcdata,
    Rawtext,
    ScriptData,
    Plaintext,
    TagOpen,
    EndTagOpen,
    TagName,
    RcdataLessThan,
    RcdataEndTagOpen,
    RcdataEndTagName,
    RawtextLessThan,
    RawtextEndTagOpen,
    RawtextEndTagName,
    ScriptDataLessThan,
    ScriptDataEndTagOpen,
    ScriptDataEndTagName,
    ScriptDataEscapeStart,
    ScriptDataEscapeStartDash,
    ScriptDataEscaped,
    ScriptDataEscapedDash,
    ScriptDataEscapedDashDash,
    ScriptDataEscapedLessThan,
    ScriptDataEscapedEndTagOpen,
    ScriptDataEscapedEndTagName,
    ScriptDataDoubleEscapeStart,
    ScriptDataDoubleEscaped,
    ScriptDataDoubleEscapedDash,
    ScriptDataDoubleEscapedDashDash,
    ScriptDataDoubleEscapedLessThan,
    ScriptDataDoubleEscapeEnd,
    BeforeAttrName,
    AttrName,
    AfterAttrName,
    BeforeAttrValue,
    AttrValueDouble,
    AttrValueSingle,
    AttrValueUnquoted,
    AfterAttrValueQuoted,
    SelfClosingStartTag,
    BogusComment,
    MarkupDeclarationOpen,
    CommentStart,
    CommentStartDash,
    Comment,
    CommentLessThan,
    CommentLessThanBang,
    CommentLessThanBangDash,
    CommentLessThanBangDashDash,
    CommentEndDash,
    CommentEnd,
    CommentEndBang,
    Doctype,
    BeforeDoctypeName,
    DoctypeName,
    AfterDoctypeName,
    AfterDoctypePublicKeyword,
    BeforeDoctypePublicId,
    DoctypePublicIdDouble,
    DoctypePublicIdSingle,
    AfterDoctypePublicId,
    BetweenDoctypePublicAndSystem,
    AfterDoctypeSystemKeyword,
    BeforeDoctypeSystemId,
    DoctypeSystemIdDouble,
    DoctypeSystemIdSingle,
    AfterDoctypeSystemId,
    BogusDoctype,
    CdataSection,
    CdataSectionBracket,
    CdataSectionEnd,
    PiOpen,
    PiTarget,
    AfterPiTarget,
    PiData,
    PiQuestionable,
    CharRef,
    NamedCharRef,
    AmbiguousAmpersand,
    NumericCharRef,
    HexCharRefStart,
    DecimalCharRefStart,
    HexCharRef,
    DecimalCharRef,
    NumericCharRefEnd,
}

pub struct Tokenizer {
    input: Vec<char>,
    pos: usize,
    pub state: State,
    return_state: State,
    out: VecDeque<Token>,
    tag: Tag,
    is_end: bool,
    attr_name: String,
    attr_value: String,
    in_attr: bool,
    comment: String,
    doctype: Doctype,
    temp: String,
    char_ref_code: u32,
    pub last_start_tag: String,
    /// set by the tree builder: the adjusted current node is not an HTML element
    pub allow_cdata: bool,
    pub errors: usize,
    eof_emitted: bool,
}

fn is_ws(c: char) -> bool {
    matches!(c, '\t' | '\n' | '\x0C' | ' ')
}

impl Tokenizer {
    pub fn new(src: &str) -> Self {
        // input stream preprocessing: CR LF and lone CR become LF
        let mut input = Vec::with_capacity(src.len());
        let mut it = src.chars().peekable();
        while let Some(c) = it.next() {
            if c == '\r' {
                if it.peek() == Some(&'\n') {
                    it.next();
                }
                input.push('\n');
            } else {
                input.push(c);
            }
        }
        Tokenizer {
            input,
            pos: 0,
            state: State::Data,
            return_state: State::Data,
            out: VecDeque::new(),
            tag: Tag::default(),
            is_end: false,
            attr_name: String::new(),
            attr_value: String::new(),
            in_attr: false,
            comment: String::new(),
            doctype: Doctype::default(),
            temp: String::new(),
            char_ref_code: 0,
            last_start_tag: String::new(),
            allow_cdata: false,
            errors: 0,
            eof_emitted: false,
        }
    }

    pub fn next_token(&mut self) -> Token {
        while self.out.is_empty() {
            if self.eof_emitted {
                return Token::Eof;
            }
            self.step();
        }
        self.out.pop_front().unwrap()
    }

    fn consume(&mut self) -> Option<char> {
        let c = self.input.get(self.pos).copied();
        self.pos += 1;
        c
    }
    fn reconsume(&mut self, s: State) {
        self.pos -= 1;
        self.state = s;
    }
    fn err(&mut self) {
        self.errors += 1;
    }
    fn emit(&mut self, t: Token) {
        if t == Token::Eof {
            self.eof_emitted = true;
        }
        self.out.push_back(t);
    }
    fn emit_char(&mut self, c: char) {
        self.out.push_back(Token::Char(c));
    }
    fn emit_str(&mut self, s: &str) {
        for c in s.chars() {
            self.emit_char(c);
        }
    }
    /// true (and consumes) if the next characters match `s` (ASCII case-insensitively if `ci`)
    fn lookahead(&mut self, s: &str, ci: bool) -> bool {
        let n = s.chars().count();
        if self.pos + n > self.input.len() {
            return false;
        }
        let ok = s.chars().zip(&self.input[self.pos..self.pos + n]).all(|(a, &b)| {
            if ci {
                a.eq_ignore_ascii_case(&b)
            } else {
                a == b
            }
        });
        if ok {
            self.pos += n;
        }
        ok
    }

    fn new_tag(&mut self, end: bool) {
        self.tag = Tag::default();
        self.is_end = end;
    }
    fn start_attr(&mut self) {
        self.finish_attr();
        self.in_attr = true;
    }
    fn finish_attr(&mut self) {
        if self.in_attr {
            self.in_attr = false;
            let name = std::mem::take(&mut self.attr_name);
            let value = std::mem::take(&mut self.attr_value);
            if self.tag.attrs.iter().any(|a| a.name == name) {
                self.err(); // duplicate attribute: dropped
            } else {
                self.tag.attrs.push(Attr { name, value });
            }
        }
    }
    fn emit_tag(&mut self) {
        self.finish_attr();
        let t = std::mem::take(&mut self.tag);
        if self.is_end {
            if !t.attrs.is_empty() || t.self_closing {
                self.err();
            }
            self.emit(Token::EndTag(t));
        } else {
            self.last_start_tag = t.name.clone();
            self.emit(Token::StartTag(t));
        }
    }
    fn appropriate_end(&self) -> bool {
        self.is_end && !self.last_start_tag.is_empty() && self.tag.name == self.last_start_tag
    }
    fn emit_comment(&mut self) {
        let c = std::mem::take(&mut self.comment);
        self.emit(Token::Comment(c));
    }
    fn emit_doctype(&mut self) {
        let d = std::mem::take(&mut self.doctype);
        self.emit(Token::Doctype(d));
    }
    fn char_ref_in_attr(&self) -> bool {
        matches!(
            self.return_state,
            State::AttrValueDouble | State::AttrValueSingle | State::AttrValueUnquoted
        )
    }
    fn flush_temp(&mut self) {
        let t = std::mem::take(&mut self.temp);
        if self.char_ref_in_attr() {
            self.attr_value.push_str(&t);
        } else {
            self.emit_str(&t);
        }
    }

    /// the shared shape of the RCDATA/RAWTEXT/script end-tag-name states
    fn end_tag_name(&mut self, c: Option<char>, fallback: State) {
        match c {
            Some(c) if is_ws(c) && self.appropriate_end() => self.state = State::BeforeAttrName,
            Some('/') if self.appropriate_end() => self.state = State::SelfClosingStartTag,
            Some('>') if self.appropriate_end() => {
                self.state = State::Data;
                self.emit_tag();
            }
            Some(c) if c.is_ascii_alphabetic() => {
                self.tag.name.push(c.to_ascii_lowercase());
                self.temp.push(c);
            }
            _ => {
                self.emit_str("</");
                let t = std::mem::take(&mut self.temp);
                self.emit_str(&t);
                self.reconsume(fallback);
            }
        }
    }

    fn step(&mut self) {
        use State::*;
        let c = self.consume();
        match self.state {
            Data => match c {
                Some('&') => {
                    self.return_state = Data;
                    self.state = CharRef;
                }
                Some('<') => self.state = TagOpen,
                Some('\0') => {
                    self.err();
                    self.emit_char('\0');
                }
                Some(c) => self.emit_char(c),
                None => self.emit(Token::Eof),
            },
            Rcdata => match c {
                Some('&') => {
                    self.return_state = Rcdata;
                    self.state = CharRef;
                }
                Some('<') => self.state = RcdataLessThan,
                Some('\0') => {
                    self.err();
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => self.emit_char(c),
                None => self.emit(Token::Eof),
            },
            Rawtext => match c {
                Some('<') => self.state = RawtextLessThan,
                Some('\0') => {
                    self.err();
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => self.emit_char(c),
                None => self.emit(Token::Eof),
            },
            ScriptData => match c {
                Some('<') => self.state = ScriptDataLessThan,
                Some('\0') => {
                    self.err();
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => self.emit_char(c),
                None => self.emit(Token::Eof),
            },
            Plaintext => match c {
                Some('\0') => {
                    self.err();
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => self.emit_char(c),
                None => self.emit(Token::Eof),
            },
            TagOpen => match c {
                Some('!') => self.state = MarkupDeclarationOpen,
                Some('/') => self.state = EndTagOpen,
                Some(c) if c.is_ascii_alphabetic() => {
                    self.new_tag(false);
                    self.reconsume(TagName);
                }
                Some('?') => {
                    self.temp.clear();
                    self.state = PiOpen;
                }
                None => {
                    self.err();
                    self.emit_char('<');
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.emit_char('<');
                    self.reconsume(Data);
                }
            },
            EndTagOpen => match c {
                Some(c) if c.is_ascii_alphabetic() => {
                    self.new_tag(true);
                    self.reconsume(TagName);
                }
                Some('>') => {
                    self.err();
                    self.state = Data;
                }
                None => {
                    self.err();
                    self.emit_str("</");
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.comment.clear();
                    self.reconsume(BogusComment);
                }
            },
            TagName => match c {
                Some(c) if is_ws(c) => self.state = BeforeAttrName,
                Some('/') => self.state = SelfClosingStartTag,
                Some('>') => {
                    self.state = Data;
                    self.emit_tag();
                }
                Some('\0') => {
                    self.err();
                    self.tag.name.push('\u{FFFD}');
                }
                Some(c) => self.tag.name.push(c.to_ascii_lowercase()),
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            RcdataLessThan => match c {
                Some('/') => {
                    self.temp.clear();
                    self.state = RcdataEndTagOpen;
                }
                _ => {
                    self.emit_char('<');
                    self.reconsume(Rcdata);
                }
            },
            RcdataEndTagOpen => match c {
                Some(c) if c.is_ascii_alphabetic() => {
                    self.new_tag(true);
                    self.reconsume(RcdataEndTagName);
                }
                _ => {
                    self.emit_str("</");
                    self.reconsume(Rcdata);
                }
            },
            RcdataEndTagName => self.end_tag_name(c, Rcdata),
            RawtextLessThan => match c {
                Some('/') => {
                    self.temp.clear();
                    self.state = RawtextEndTagOpen;
                }
                _ => {
                    self.emit_char('<');
                    self.reconsume(Rawtext);
                }
            },
            RawtextEndTagOpen => match c {
                Some(c) if c.is_ascii_alphabetic() => {
                    self.new_tag(true);
                    self.reconsume(RawtextEndTagName);
                }
                _ => {
                    self.emit_str("</");
                    self.reconsume(Rawtext);
                }
            },
            RawtextEndTagName => self.end_tag_name(c, Rawtext),
            ScriptDataLessThan => match c {
                Some('/') => {
                    self.temp.clear();
                    self.state = ScriptDataEndTagOpen;
                }
                Some('!') => {
                    self.state = ScriptDataEscapeStart;
                    self.emit_str("<!");
                }
                _ => {
                    self.emit_char('<');
                    self.reconsume(ScriptData);
                }
            },
            ScriptDataEndTagOpen => match c {
                Some(c) if c.is_ascii_alphabetic() => {
                    self.new_tag(true);
                    self.reconsume(ScriptDataEndTagName);
                }
                _ => {
                    self.emit_str("</");
                    self.reconsume(ScriptData);
                }
            },
            ScriptDataEndTagName => self.end_tag_name(c, ScriptData),
            ScriptDataEscapeStart => match c {
                Some('-') => {
                    self.state = ScriptDataEscapeStartDash;
                    self.emit_char('-');
                }
                _ => self.reconsume(ScriptData),
            },
            ScriptDataEscapeStartDash => match c {
                Some('-') => {
                    self.state = ScriptDataEscapedDashDash;
                    self.emit_char('-');
                }
                _ => self.reconsume(ScriptData),
            },
            ScriptDataEscaped => match c {
                Some('-') => {
                    self.state = ScriptDataEscapedDash;
                    self.emit_char('-');
                }
                Some('<') => self.state = ScriptDataEscapedLessThan,
                Some('\0') => {
                    self.err();
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => self.emit_char(c),
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            ScriptDataEscapedDash => match c {
                Some('-') => {
                    self.state = ScriptDataEscapedDashDash;
                    self.emit_char('-');
                }
                Some('<') => self.state = ScriptDataEscapedLessThan,
                Some('\0') => {
                    self.err();
                    self.state = ScriptDataEscaped;
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => {
                    self.state = ScriptDataEscaped;
                    self.emit_char(c);
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            ScriptDataEscapedDashDash => match c {
                Some('-') => self.emit_char('-'),
                Some('<') => self.state = ScriptDataEscapedLessThan,
                Some('>') => {
                    self.state = ScriptData;
                    self.emit_char('>');
                }
                Some('\0') => {
                    self.err();
                    self.state = ScriptDataEscaped;
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => {
                    self.state = ScriptDataEscaped;
                    self.emit_char(c);
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            ScriptDataEscapedLessThan => match c {
                Some('/') => {
                    self.temp.clear();
                    self.state = ScriptDataEscapedEndTagOpen;
                }
                Some(c) if c.is_ascii_alphabetic() => {
                    self.temp.clear();
                    self.emit_char('<');
                    self.reconsume(ScriptDataDoubleEscapeStart);
                }
                _ => {
                    self.emit_char('<');
                    self.reconsume(ScriptDataEscaped);
                }
            },
            ScriptDataEscapedEndTagOpen => match c {
                Some(c) if c.is_ascii_alphabetic() => {
                    self.new_tag(true);
                    self.reconsume(ScriptDataEscapedEndTagName);
                }
                _ => {
                    self.emit_str("</");
                    self.reconsume(ScriptDataEscaped);
                }
            },
            ScriptDataEscapedEndTagName => self.end_tag_name(c, ScriptDataEscaped),
            ScriptDataDoubleEscapeStart => match c {
                Some(ch) if is_ws(ch) || ch == '/' || ch == '>' => {
                    self.state = if self.temp == "script" { ScriptDataDoubleEscaped } else { ScriptDataEscaped };
                    self.emit_char(ch);
                }
                Some(ch) if ch.is_ascii_alphabetic() => {
                    self.temp.push(ch.to_ascii_lowercase());
                    self.emit_char(ch);
                }
                _ => self.reconsume(ScriptDataEscaped),
            },
            ScriptDataDoubleEscaped => match c {
                Some('-') => {
                    self.state = ScriptDataDoubleEscapedDash;
                    self.emit_char('-');
                }
                Some('<') => {
                    self.state = ScriptDataDoubleEscapedLessThan;
                    self.emit_char('<');
                }
                Some('\0') => {
                    self.err();
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => self.emit_char(c),
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            ScriptDataDoubleEscapedDash => match c {
                Some('-') => {
                    self.state = ScriptDataDoubleEscapedDashDash;
                    self.emit_char('-');
                }
                Some('<') => {
                    self.state = ScriptDataDoubleEscapedLessThan;
                    self.emit_char('<');
                }
                Some('\0') => {
                    self.err();
                    self.state = ScriptDataDoubleEscaped;
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => {
                    self.state = ScriptDataDoubleEscaped;
                    self.emit_char(c);
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            ScriptDataDoubleEscapedDashDash => match c {
                Some('-') => self.emit_char('-'),
                Some('<') => {
                    self.state = ScriptDataDoubleEscapedLessThan;
                    self.emit_char('<');
                }
                Some('>') => {
                    self.state = ScriptData;
                    self.emit_char('>');
                }
                Some('\0') => {
                    self.err();
                    self.state = ScriptDataDoubleEscaped;
                    self.emit_char('\u{FFFD}');
                }
                Some(c) => {
                    self.state = ScriptDataDoubleEscaped;
                    self.emit_char(c);
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            ScriptDataDoubleEscapedLessThan => match c {
                Some('/') => {
                    self.temp.clear();
                    self.state = ScriptDataDoubleEscapeEnd;
                    self.emit_char('/');
                }
                _ => self.reconsume(ScriptDataDoubleEscaped),
            },
            ScriptDataDoubleEscapeEnd => match c {
                Some(ch) if is_ws(ch) || ch == '/' || ch == '>' => {
                    self.state = if self.temp == "script" { ScriptDataEscaped } else { ScriptDataDoubleEscaped };
                    self.emit_char(ch);
                }
                Some(ch) if ch.is_ascii_alphabetic() => {
                    self.temp.push(ch.to_ascii_lowercase());
                    self.emit_char(ch);
                }
                _ => self.reconsume(ScriptDataDoubleEscaped),
            },
            BeforeAttrName => match c {
                Some(c) if is_ws(c) => {}
                Some('/') | Some('>') | None => self.reconsume(AfterAttrName),
                Some('=') => {
                    self.err();
                    self.start_attr();
                    self.attr_name.push('=');
                    self.state = AttrName;
                }
                Some(_) => {
                    self.start_attr();
                    self.reconsume(AttrName);
                }
            },
            AttrName => match c {
                Some(c) if is_ws(c) || c == '/' || c == '>' => self.reconsume(AfterAttrName),
                None => self.reconsume(AfterAttrName),
                Some('=') => self.state = BeforeAttrValue,
                Some('\0') => {
                    self.err();
                    self.attr_name.push('\u{FFFD}');
                }
                Some(c) => {
                    if c == '"' || c == '\'' || c == '<' {
                        self.err();
                    }
                    self.attr_name.push(c.to_ascii_lowercase());
                }
            },
            AfterAttrName => match c {
                Some(c) if is_ws(c) => {}
                Some('/') => self.state = SelfClosingStartTag,
                Some('=') => self.state = BeforeAttrValue,
                Some('>') => {
                    self.state = Data;
                    self.emit_tag();
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.start_attr();
                    self.reconsume(AttrName);
                }
            },
            BeforeAttrValue => match c {
                Some(c) if is_ws(c) => {}
                Some('"') => self.state = AttrValueDouble,
                Some('\'') => self.state = AttrValueSingle,
                Some('>') => {
                    self.err();
                    self.state = Data;
                    self.emit_tag();
                }
                _ => self.reconsume(AttrValueUnquoted),
            },
            AttrValueDouble | AttrValueSingle => {
                let q = if self.state == AttrValueDouble { '"' } else { '\'' };
                match c {
                    Some(ch) if ch == q => self.state = AfterAttrValueQuoted,
                    Some('&') => {
                        self.return_state = self.state;
                        self.state = CharRef;
                    }
                    Some('\0') => {
                        self.err();
                        self.attr_value.push('\u{FFFD}');
                    }
                    Some(ch) => self.attr_value.push(ch),
                    None => {
                        self.err();
                        self.emit(Token::Eof);
                    }
                }
            }
            AttrValueUnquoted => match c {
                Some(c) if is_ws(c) => self.state = BeforeAttrName,
                Some('&') => {
                    self.return_state = AttrValueUnquoted;
                    self.state = CharRef;
                }
                Some('>') => {
                    self.state = Data;
                    self.emit_tag();
                }
                Some('\0') => {
                    self.err();
                    self.attr_value.push('\u{FFFD}');
                }
                Some(c) => {
                    if matches!(c, '"' | '\'' | '<' | '=' | '`') {
                        self.err();
                    }
                    self.attr_value.push(c);
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            AfterAttrValueQuoted => match c {
                Some(c) if is_ws(c) => self.state = BeforeAttrName,
                Some('/') => self.state = SelfClosingStartTag,
                Some('>') => {
                    self.state = Data;
                    self.emit_tag();
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.reconsume(BeforeAttrName);
                }
            },
            SelfClosingStartTag => match c {
                Some('>') => {
                    self.tag.self_closing = true;
                    self.state = Data;
                    self.emit_tag();
                }
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.reconsume(BeforeAttrName);
                }
            },
            BogusComment => match c {
                Some('>') => {
                    self.state = Data;
                    self.emit_comment();
                }
                None => {
                    self.emit_comment();
                    self.emit(Token::Eof);
                }
                Some('\0') => {
                    self.err();
                    self.comment.push('\u{FFFD}');
                }
                Some(c) => self.comment.push(c),
            },
            MarkupDeclarationOpen => {
                self.pos -= 1;
                if self.lookahead("--", false) {
                    self.comment.clear();
                    self.state = CommentStart;
                } else if self.lookahead("DOCTYPE", true) {
                    self.state = Doctype;
                } else if self.lookahead("[CDATA[", false) {
                    if self.allow_cdata {
                        self.state = CdataSection;
                    } else {
                        self.err();
                        self.comment = "[CDATA[".into();
                        self.state = BogusComment;
                    }
                } else {
                    self.err();
                    self.comment.clear();
                    self.state = BogusComment;
                }
            }
            CommentStart => match c {
                Some('-') => self.state = CommentStartDash,
                Some('>') => {
                    self.err();
                    self.state = Data;
                    self.emit_comment();
                }
                _ => self.reconsume(Comment),
            },
            CommentStartDash => match c {
                Some('-') => self.state = CommentEnd,
                Some('>') => {
                    self.err();
                    self.state = Data;
                    self.emit_comment();
                }
                None => {
                    self.err();
                    self.emit_comment();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.comment.push('-');
                    self.reconsume(Comment);
                }
            },
            Comment => match c {
                Some('<') => {
                    self.comment.push('<');
                    self.state = CommentLessThan;
                }
                Some('-') => self.state = CommentEndDash,
                Some('\0') => {
                    self.err();
                    self.comment.push('\u{FFFD}');
                }
                Some(c) => self.comment.push(c),
                None => {
                    self.err();
                    self.emit_comment();
                    self.emit(Token::Eof);
                }
            },
            CommentLessThan => match c {
                Some('!') => {
                    self.comment.push('!');
                    self.state = CommentLessThanBang;
                }
                Some('<') => self.comment.push('<'),
                _ => self.reconsume(Comment),
            },
            CommentLessThanBang => match c {
                Some('-') => self.state = CommentLessThanBangDash,
                _ => self.reconsume(Comment),
            },
            CommentLessThanBangDash => match c {
                Some('-') => self.state = CommentLessThanBangDashDash,
                _ => self.reconsume(CommentEndDash),
            },
            CommentLessThanBangDashDash => match c {
                Some('>') | None => self.reconsume(CommentEnd),
                _ => {
                    self.err();
                    self.reconsume(CommentEnd);
                }
            },
            CommentEndDash => match c {
                Some('-') => self.state = CommentEnd,
                None => {
                    self.err();
                    self.emit_comment();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.comment.push('-');
                    self.reconsume(Comment);
                }
            },
            CommentEnd => match c {
                Some('>') => {
                    self.state = Data;
                    self.emit_comment();
                }
                Some('!') => self.state = CommentEndBang,
                Some('-') => self.comment.push('-'),
                None => {
                    self.err();
                    self.emit_comment();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.comment.push_str("--");
                    self.reconsume(Comment);
                }
            },
            CommentEndBang => match c {
                Some('-') => {
                    self.comment.push_str("--!");
                    self.state = CommentEndDash;
                }
                Some('>') => {
                    self.err();
                    self.state = Data;
                    self.emit_comment();
                }
                None => {
                    self.err();
                    self.emit_comment();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.comment.push_str("--!");
                    self.reconsume(Comment);
                }
            },
            Doctype => match c {
                Some(c) if is_ws(c) => self.state = BeforeDoctypeName,
                Some('>') => self.reconsume(BeforeDoctypeName),
                None => {
                    self.err();
                    self.doctype = crate::html::tokenizer::Doctype { force_quirks: true, ..Default::default() };
                    self.emit_doctype();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.reconsume(BeforeDoctypeName);
                }
            },
            BeforeDoctypeName => match c {
                Some(c) if is_ws(c) => {}
                Some('\0') => {
                    self.err();
                    self.doctype = Default::default();
                    self.doctype.name = Some("\u{FFFD}".into());
                    self.state = DoctypeName;
                }
                Some('>') => {
                    self.err();
                    self.doctype = Default::default();
                    self.doctype.force_quirks = true;
                    self.state = Data;
                    self.emit_doctype();
                }
                None => {
                    self.err();
                    self.doctype = Default::default();
                    self.doctype.force_quirks = true;
                    self.emit_doctype();
                    self.emit(Token::Eof);
                }
                Some(c) => {
                    self.doctype = Default::default();
                    self.doctype.name = Some(c.to_ascii_lowercase().to_string());
                    self.state = DoctypeName;
                }
            },
            DoctypeName => match c {
                Some(c) if is_ws(c) => self.state = AfterDoctypeName,
                Some('>') => {
                    self.state = Data;
                    self.emit_doctype();
                }
                Some('\0') => {
                    self.err();
                    self.doctype.name.as_mut().unwrap().push('\u{FFFD}');
                }
                Some(c) => self.doctype.name.as_mut().unwrap().push(c.to_ascii_lowercase()),
                None => {
                    self.err();
                    self.doctype.force_quirks = true;
                    self.emit_doctype();
                    self.emit(Token::Eof);
                }
            },
            AfterDoctypeName => match c {
                Some(c) if is_ws(c) => {}
                Some('>') => {
                    self.state = Data;
                    self.emit_doctype();
                }
                None => {
                    self.err();
                    self.doctype.force_quirks = true;
                    self.emit_doctype();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.pos -= 1;
                    if self.lookahead("PUBLIC", true) {
                        self.state = AfterDoctypePublicKeyword;
                    } else if self.lookahead("SYSTEM", true) {
                        self.state = AfterDoctypeSystemKeyword;
                    } else {
                        self.pos += 1;
                        self.err();
                        self.doctype.force_quirks = true;
                        self.reconsume(BogusDoctype);
                    }
                }
            },
            AfterDoctypePublicKeyword | BeforeDoctypePublicId | AfterDoctypeSystemKeyword | BeforeDoctypeSystemId => {
                let public = matches!(self.state, AfterDoctypePublicKeyword | BeforeDoctypePublicId);
                let after_kw = matches!(self.state, AfterDoctypePublicKeyword | AfterDoctypeSystemKeyword);
                match c {
                    Some(c) if is_ws(c) => {
                        if after_kw {
                            self.state = if public { BeforeDoctypePublicId } else { BeforeDoctypeSystemId };
                        }
                    }
                    Some(q @ ('"' | '\'')) => {
                        if after_kw {
                            self.err();
                        }
                        if public {
                            self.doctype.public_id = Some(String::new());
                            self.state = if q == '"' { DoctypePublicIdDouble } else { DoctypePublicIdSingle };
                        } else {
                            self.doctype.system_id = Some(String::new());
                            self.state = if q == '"' { DoctypeSystemIdDouble } else { DoctypeSystemIdSingle };
                        }
                    }
                    Some('>') => {
                        self.err();
                        self.doctype.force_quirks = true;
                        self.state = Data;
                        self.emit_doctype();
                    }
                    None => {
                        self.err();
                        self.doctype.force_quirks = true;
                        self.emit_doctype();
                        self.emit(Token::Eof);
                    }
                    Some(_) => {
                        self.err();
                        self.doctype.force_quirks = true;
                        self.reconsume(BogusDoctype);
                    }
                }
            }
            DoctypePublicIdDouble | DoctypePublicIdSingle | DoctypeSystemIdDouble | DoctypeSystemIdSingle => {
                let public = matches!(self.state, DoctypePublicIdDouble | DoctypePublicIdSingle);
                let q = if matches!(self.state, DoctypePublicIdDouble | DoctypeSystemIdDouble) { '"' } else { '\'' };
                match c {
                    Some(ch) if ch == q => {
                        self.state = if public { AfterDoctypePublicId } else { AfterDoctypeSystemId };
                    }
                    Some('>') => {
                        self.err();
                        self.doctype.force_quirks = true;
                        self.state = Data;
                        self.emit_doctype();
                    }
                    None => {
                        self.err();
                        self.doctype.force_quirks = true;
                        self.emit_doctype();
                        self.emit(Token::Eof);
                    }
                    Some(ch) => {
                        let ch = if ch == '\0' {
                            self.err();
                            '\u{FFFD}'
                        } else {
                            ch
                        };
                        let id = if public { &mut self.doctype.public_id } else { &mut self.doctype.system_id };
                        id.as_mut().unwrap().push(ch);
                    }
                }
            }
            AfterDoctypePublicId | BetweenDoctypePublicAndSystem => {
                let after = self.state == AfterDoctypePublicId;
                match c {
                    Some(c) if is_ws(c) => {
                        if after {
                            self.state = BetweenDoctypePublicAndSystem;
                        }
                    }
                    Some('>') => {
                        self.state = Data;
                        self.emit_doctype();
                    }
                    Some(q @ ('"' | '\'')) => {
                        if after {
                            self.err();
                        }
                        self.doctype.system_id = Some(String::new());
                        self.state = if q == '"' { DoctypeSystemIdDouble } else { DoctypeSystemIdSingle };
                    }
                    None => {
                        self.err();
                        self.doctype.force_quirks = true;
                        self.emit_doctype();
                        self.emit(Token::Eof);
                    }
                    Some(_) => {
                        self.err();
                        self.doctype.force_quirks = true;
                        self.reconsume(BogusDoctype);
                    }
                }
            }
            AfterDoctypeSystemId => match c {
                Some(c) if is_ws(c) => {}
                Some('>') => {
                    self.state = Data;
                    self.emit_doctype();
                }
                None => {
                    self.err();
                    self.doctype.force_quirks = true;
                    self.emit_doctype();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.reconsume(BogusDoctype);
                }
            },
            BogusDoctype => match c {
                Some('>') => {
                    self.state = Data;
                    self.emit_doctype();
                }
                Some('\0') => self.err(),
                None => {
                    self.emit_doctype();
                    self.emit(Token::Eof);
                }
                Some(_) => {}
            },
            CdataSection => match c {
                Some(']') => self.state = CdataSectionBracket,
                Some(c) => self.emit_char(c),
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
            },
            CdataSectionBracket => match c {
                Some(']') => self.state = CdataSectionEnd,
                _ => {
                    self.emit_char(']');
                    self.reconsume(CdataSection);
                }
            },
            CdataSectionEnd => match c {
                Some(']') => self.emit_char(']'),
                Some('>') => self.state = Data,
                _ => {
                    self.emit_str("]]");
                    self.reconsume(CdataSection);
                }
            },
            PiOpen => match c {
                Some(ch) if ch.is_ascii_alphabetic() || ch == '_' => self.reconsume(PiTarget),
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.comment = format!("?{}", self.temp);
                    self.reconsume(BogusComment);
                }
            },
            PiTarget => match c {
                Some(ch) if is_ws(ch) || ch == '?' || ch == '>' => {
                    if self.temp.eq_ignore_ascii_case("xml") || self.temp.eq_ignore_ascii_case("xml-stylesheet") {
                        self.err();
                        self.comment = format!("?{}", self.temp);
                        self.reconsume(BogusComment);
                    } else {
                        self.comment.clear(); // the PI's data
                        self.reconsume(AfterPiTarget);
                    }
                }
                Some(ch) if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' => self.temp.push(ch),
                None => {
                    self.err();
                    self.emit(Token::Eof);
                }
                Some(_) => {
                    self.err();
                    self.comment = format!("?{}", self.temp);
                    self.reconsume(BogusComment);
                }
            },
            AfterPiTarget => match c {
                Some(ch) if is_ws(ch) => {}
                _ => self.reconsume(PiData),
            },
            PiData | PiQuestionable => {
                let q = self.state == PiQuestionable;
                match c {
                    Some('?') if !q => self.state = PiQuestionable,
                    Some('>') => {
                        self.state = Data;
                        let t = std::mem::take(&mut self.temp);
                        let d = std::mem::take(&mut self.comment);
                        self.emit(Token::Pi(t, d));
                    }
                    None => {
                        self.err();
                        self.emit(Token::Eof);
                    }
                    Some(ch) => {
                        if q {
                            self.comment.push('?');
                            self.reconsume(PiData);
                        } else {
                            self.comment.push(ch);
                        }
                    }
                }
            }
            CharRef => {
                self.temp.clear();
                self.temp.push('&');
                match c {
                    Some(c) if c.is_ascii_alphanumeric() => self.reconsume(NamedCharRef),
                    Some('#') => {
                        self.temp.push('#');
                        self.state = NumericCharRef;
                    }
                    _ => {
                        self.flush_temp();
                        let r = self.return_state;
                        self.reconsume(r);
                    }
                }
            }
            NamedCharRef => {
                self.pos -= 1;
                let m = entities::longest_match(&self.input[self.pos..]);
                match m {
                    Some((n, chars)) => {
                        let name: String = self.input[self.pos..self.pos + n].iter().collect();
                        self.pos += n;
                        let last_semi = name.ends_with(';');
                        let next = self.input.get(self.pos).copied();
                        if self.char_ref_in_attr()
                            && !last_semi
                            && matches!(next, Some(ch) if ch == '=' || ch.is_ascii_alphanumeric())
                        {
                            self.temp.push_str(&name);
                            self.flush_temp();
                        } else {
                            if !last_semi {
                                self.err();
                            }
                            self.temp = chars.to_string();
                            self.flush_temp();
                        }
                        self.state = self.return_state;
                    }
                    None => {
                        self.flush_temp();
                        self.state = AmbiguousAmpersand;
                    }
                }
            }
            AmbiguousAmpersand => match c {
                Some(ch) if ch.is_ascii_alphanumeric() => {
                    if self.char_ref_in_attr() {
                        self.attr_value.push(ch);
                    } else {
                        self.emit_char(ch);
                    }
                }
                Some(';') => {
                    self.err();
                    let r = self.return_state;
                    self.reconsume(r);
                }
                _ => {
                    let r = self.return_state;
                    self.reconsume(r);
                }
            },
            NumericCharRef => {
                self.char_ref_code = 0;
                match c {
                    Some(ch @ ('x' | 'X')) => {
                        self.temp.push(ch);
                        self.state = HexCharRefStart;
                    }
                    _ => self.reconsume(DecimalCharRefStart),
                }
            }
            HexCharRefStart => match c {
                Some(ch) if ch.is_ascii_hexdigit() => self.reconsume(HexCharRef),
                _ => {
                    self.err();
                    self.flush_temp();
                    let r = self.return_state;
                    self.reconsume(r);
                }
            },
            DecimalCharRefStart => match c {
                Some(ch) if ch.is_ascii_digit() => self.reconsume(DecimalCharRef),
                _ => {
                    self.err();
                    self.flush_temp();
                    let r = self.return_state;
                    self.reconsume(r);
                }
            },
            HexCharRef | DecimalCharRef => {
                let radix = if self.state == HexCharRef { 16 } else { 10 };
                match c {
                    Some(ch) if ch.is_digit(radix) => {
                        self.char_ref_code = self
                            .char_ref_code
                            .saturating_mul(radix)
                            .saturating_add(ch.to_digit(radix).unwrap())
                            .min(0x110000);
                    }
                    Some(';') => self.state = NumericCharRefEnd,
                    _ => {
                        self.err();
                        self.reconsume(NumericCharRefEnd);
                    }
                }
            }
            NumericCharRefEnd => {
                self.pos -= 1;
                let mut n = self.char_ref_code;
                if n == 0 || n > 0x10FFFF || (0xD800..=0xDFFF).contains(&n) {
                    self.err();
                    n = 0xFFFD;
                } else if let Some(r) = c1_replacement(n) {
                    self.err();
                    n = r;
                } else if (0xFDD0..=0xFDEF).contains(&n) || (n & 0xFFFE) == 0xFFFE || n == 0x0D || (n < 0x20 && !is_ws(char::from_u32(n).unwrap())) || (0x7F..=0x9F).contains(&n) {
                    self.err(); // noncharacter or control: kept, but an error
                }
                self.temp = char::from_u32(n).unwrap().to_string();
                self.flush_temp();
                self.state = self.return_state;
            }
        }
    }
}

fn c1_replacement(n: u32) -> Option<u32> {
    Some(match n {
        0x80 => 0x20AC,
        0x82 => 0x201A,
        0x83 => 0x0192,
        0x84 => 0x201E,
        0x85 => 0x2026,
        0x86 => 0x2020,
        0x87 => 0x2021,
        0x88 => 0x02C6,
        0x89 => 0x2030,
        0x8A => 0x0160,
        0x8B => 0x2039,
        0x8C => 0x0152,
        0x8E => 0x017D,
        0x91 => 0x2018,
        0x92 => 0x2019,
        0x93 => 0x201C,
        0x94 => 0x201D,
        0x95 => 0x2022,
        0x96 => 0x2013,
        0x97 => 0x2014,
        0x98 => 0x02DC,
        0x99 => 0x2122,
        0x9A => 0x0161,
        0x9B => 0x203A,
        0x9C => 0x0153,
        0x9E => 0x017E,
        0x9F => 0x0178,
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn toks(s: &str) -> Vec<Token> {
        let mut t = Tokenizer::new(s);
        let mut v = vec![];
        loop {
            let x = t.next_token();
            if x == Token::Eof {
                return v;
            }
            v.push(x);
        }
    }

    #[test]
    fn tags_attrs_refs() {
        let v = toks("<a href='x&amp;y' b>t&lt;</a>");
        let Token::StartTag(t) = &v[0] else { panic!() };
        assert_eq!(t.name, "a");
        assert_eq!(t.attr("href"), Some("x&y"));
        assert_eq!(t.attr("b"), Some(""));
        assert_eq!(v[1], Token::Char('t'));
        assert_eq!(v[2], Token::Char('<'));
        assert!(matches!(&v[3], Token::EndTag(t) if t.name == "a"));
    }

    #[test]
    fn rawtext_end_tag() {
        let mut t = Tokenizer::new("a</b></style>");
        t.state = State::Rawtext;
        t.last_start_tag = "style".into();
        let mut s = String::new();
        loop {
            match t.next_token() {
                Token::Char(c) => s.push(c),
                Token::EndTag(e) => {
                    assert_eq!(e.name, "style");
                    break;
                }
                x => panic!("{x:?}"),
            }
        }
        assert_eq!(s, "a</b>");
    }
}
