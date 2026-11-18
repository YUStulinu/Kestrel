//! The named character references of the HTML standard, read from the
//! standard's own entities.json (included at build time).
use std::collections::HashMap;
use std::sync::OnceLock;

static JSON: &str = include_str!("entities.json");

/// name without the leading '&' (with or without the trailing ';') -> characters
pub fn table() -> &'static HashMap<String, String> {
    static T: OnceLock<HashMap<String, String>> = OnceLock::new();
    T.get_or_init(|| {
        let mut m = HashMap::new();
        // entries look like  "&AElig;": { "codepoints": [198], "characters": "Æ" },
        let mut rest = JSON;
        while let Some(i) = rest.find("\"&") {
            rest = &rest[i + 2..];
            let end = rest.find('"').unwrap();
            let name = rest[..end].to_string();
            let cp = rest.find("[").unwrap();
            let cpe = rest.find("]").unwrap();
            let s: String = rest[cp + 1..cpe]
                .split(',')
                .filter_map(|x| x.trim().parse::<u32>().ok())
                .filter_map(char::from_u32)
                .collect();
            m.insert(name, s);
            rest = &rest[cpe..];
        }
        m
    })
}

/// the longest entity name (without '&') that prefixes `s`
pub fn longest_match(s: &[char]) -> Option<(usize, &'static str)> {
    let t = table();
    let mut buf = String::new();
    let mut best = None;
    for (i, &c) in s.iter().enumerate().take(40) {
        buf.push(c);
        if let Some(v) = t.get(&buf) {
            best = Some((i + 1, v.as_str()));
        }
        if !(c.is_ascii_alphanumeric()) {
            break;
        }
    }
    best
}

#[cfg(test)]
mod tests {
    #[test]
    fn table_and_match() {
        assert_eq!(super::table().len(), 2231);
        let s: Vec<char> = "notin;x".chars().collect();
        assert_eq!(super::longest_match(&s), Some((6, "\u{2209}")));
        let s: Vec<char> = "notit;".chars().collect();
        assert_eq!(super::longest_match(&s), Some((3, "\u{AC}")));
    }
}
