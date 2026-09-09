//! Decoding control-API response bodies that are not guaranteed to be UTF-8.
//!
//! `nxt_conf_json_parse_string()` keeps every byte from 0x20 upwards that it
//! finds inside a JSON string, so a configuration stored by any released
//! version -- 1.34 LTS and every 1.36.x -- can hold bytes that are not valid
//! UTF-8, and `GET /config` hands them back verbatim.  `serde_json` rejects
//! such a body whole, which would leave unitctl unable to look at the one
//! configuration it is most needed for.
//!
//! Everything here is a fallback.  A body that is valid UTF-8 takes the path
//! it always took, byte for byte; only a body that cannot be decoded strictly
//! is decoded again with the undecodable bytes replaced, and that never
//! happens silently.

use serde::de::{DeserializeOwned, MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};
use serde_json::Value;
use std::fmt;

/// The character `String::from_utf8_lossy` leaves behind.
const REPLACEMENT: char = '\u{fffd}';

/// How many members a warning names before it stops listing them.
const MAX_LISTED: usize = 8;

/// How much of the server's answer survived decoding.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum Fidelity {
    /// The body was valid UTF-8: the value is exactly what the server sent.
    #[default]
    Exact,
    /// The body was not valid UTF-8.  Every byte that could not be decoded was
    /// replaced with U+FFFD, so the value is readable but is *not* what the
    /// server stores.  `members` holds the RFC 6901 pointers of the members
    /// that carry a replacement character.
    Replaced { members: Vec<String> },
}

impl Fidelity {
    pub fn is_exact(&self) -> bool {
        matches!(self, Fidelity::Exact)
    }

    /// The affected members, as a comma-separated list of RFC 6901 pointers.
    pub fn members(&self) -> String {
        let members = match self {
            Fidelity::Exact => return String::new(),
            Fidelity::Replaced { members } => members,
        };

        if members.is_empty() {
            // The bytes did not survive as a string value, so there is nothing
            // to point at.  Say that rather than printing an empty list.
            return "an unlocatable part of the document".to_string();
        }

        let listed = members
            .iter()
            .take(MAX_LISTED)
            .map(|member| {
                if member.is_empty() {
                    "the whole document".to_string()
                } else {
                    one_line(member)
                }
            })
            .collect::<Vec<_>>()
            .join(", ");

        match members.len().checked_sub(MAX_LISTED) {
            Some(rest) if rest > 0 => format!("{} and {} more", listed, rest),
            _ => listed,
        }
    }

    /// The warning to print when a body had to be decoded lossily, or `None`
    /// when it did not.
    pub fn warning(&self, path: &str) -> Option<String> {
        if self.is_exact() {
            return None;
        }

        Some(format!(
            "Warning: {} returned bytes that are not valid UTF-8.\n\
             Warning: they are shown as U+FFFD and are NOT what the server stores: {}",
            path,
            self.members()
        ))
    }
}

/// How much of a response body that would not parse is shown.
const MAX_SHOWN_BODY: usize = 2048;

/// How a control character is written when text from the server is printed.
///
/// Deliberately not U+FFFD.  That character is what [`Fidelity::warning`] tells
/// the operator is a byte the server stores and UTF-8 cannot carry, and a
/// control character in a name is not one: it is valid UTF-8 that arrived
/// intact, so showing it as U+FFFD would send the operator hunting a byte that
/// does not exist -- and `unitctl_error.rs`'s advice on such a member would be
/// wrong.  The escape JSON itself would use keeps the two apart and still says
/// what was really there, which a `?` would throw away.
fn escaped(character: char) -> String {
    format!("\\u{:04x}", character as u32)
}

/// One line of text from the server, made safe to print.
///
/// A member name can hold any character JSON can spell, and `serde_json` decodes
/// the escape for ESC into a real ESC: a name can carry an ANSI escape sequence
/// that the terminal would act on, or a newline or carriage return, which would
/// let it forge a line of unitctl's own output.  Only a raw control *byte* is
/// refused by the parser, and a name reaches stderr through every warning about
/// replaced bytes, so every control character is written out by [`escaped`].
///
/// Every one of them, newline and tab included, because a pointer is one line.
/// [`body_for_display`] keeps those two, because a page is many lines.
pub(crate) fn one_line(text: &str) -> String {
    let mut shown = String::with_capacity(text.len());

    for character in text.chars() {
        if character.is_control() {
            shown.push_str(&escaped(character));
        } else {
            shown.push(character);
        }
    }

    shown
}

/// Render a response body that would not parse, for a terminal.
///
/// Whatever answered the request wrote this: an intermediary's page, a truncated
/// answer, a captive portal's sign-in form.  It is neither bounded -- an error
/// page can be megabytes -- nor trusted, and the control characters in it are
/// the ones a terminal acts on, so an escape sequence in a 502 page would
/// otherwise reach the user's screen.
///
/// Newline and tab are kept, because a page is many lines.  Every other control
/// character is written out by [`escaped`], and the U+FFFD that can appear here
/// is the real thing: this decodes lossily, so those are bytes that could not be
/// decoded.
pub fn body_for_display(bytes: &[u8]) -> String {
    // Rendering never shortens: a character of n bytes comes back as n bytes, or
    // as the 3 of U+FFFD, or as the 6 of an escape.  So this many input bytes
    // always suffice to fill the budget, and stopping there is what keeps a
    // multi-megabyte page from being copied whole to show 2 KiB of it.  The few
    // extra bytes cover a character the cut lands inside.
    let head = &bytes[..bytes.len().min(MAX_SHOWN_BODY + 4)];
    let mut shown = String::new();

    for character in String::from_utf8_lossy(head).chars() {
        let keep = match character {
            '\n' | '\t' => character.to_string(),
            control if control.is_control() => escaped(control),
            other => other.to_string(),
        };

        if shown.len() + keep.len() > MAX_SHOWN_BODY {
            // Said of the output, because that is what was measured: a page of
            // control or undecodable bytes renders to several bytes per byte, so
            // this is not a count of the body.
            shown.push_str(&format!("\n[... truncated after {} bytes of output]", MAX_SHOWN_BODY));
            return shown;
        }

        shown.push_str(&keep);
    }

    shown
}

/// Deserialize a control-API response body, tolerating bytes that are not
/// valid UTF-8.
///
/// The strict decode is tried first and is what almost every body takes, so a
/// server that answers in UTF-8 is unaffected.  Only when the bytes themselves
/// are not UTF-8 is the body decoded again with the undecodable bytes
/// replaced; a body that is valid UTF-8 but malformed JSON, or that does not
/// match `T`, still fails with the error it always failed with.
///
/// A replacing decode that would lose a member fails instead.  See
/// [`Unambiguous`]: the point of the fallback is to show the operator what is
/// there, so showing less than what is there without saying so is the one
/// outcome it must not have.
pub fn decode<T: DeserializeOwned>(bytes: &[u8]) -> Result<(T, Fidelity), serde_json::Error> {
    match serde_json::from_slice::<T>(bytes) {
        Ok(value) => Ok((value, Fidelity::Exact)),
        Err(strict) => {
            if std::str::from_utf8(bytes).is_ok() {
                return Err(strict);
            }

            let text = String::from_utf8_lossy(bytes);
            let Unambiguous(value) = serde_json::from_str(&text)?;

            let members = replaced_members(&value);
            let typed = serde_json::from_value(value)?;

            Ok((typed, Fidelity::Replaced { members }))
        }
    }
}

/// What `decode` says when it will not show a document it knows is incomplete.
const AMBIGUOUS_NAME: &str = "two members of one object share a name, so one of them would be \
                              dropped; the usual reason is names that differ only in bytes that are \
                              not valid UTF-8, which decode alike.  unitctl will not show a document \
                              it knows is incomplete: read these bytes with 'unitctl export' or curl";

/// A JSON document whose member names are known to be distinct.
///
/// `String::from_utf8_lossy` maps every undecodable byte to the same character,
/// so `"app-\xFF"` and `"app-\xFE"` arrive as one name twice.  A
/// `serde_json::Map` keeps the last of two equal names, so the other member's
/// whole value -- an application, a listener -- would be missing from what is
/// shown, and the warning about replaced bytes says nothing about the loss.
/// Deserializing through this refuses instead, at every depth.
///
/// The question is asked of the names the parser decoded, which is where the
/// answer lives.  Asking it of any rewriting of the JSON *source* cannot work,
/// because JSON's own escapes forge whatever the rewriting uses: a `%` written
/// as the escape `\u0025` is not a `%` in the source and is one after parsing.
struct Unambiguous(Value);

impl<'de> Deserialize<'de> for Unambiguous {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_any(UnambiguousVisitor)
    }
}

struct UnambiguousVisitor;

impl<'de> Visitor<'de> for UnambiguousVisitor {
    type Value = Unambiguous;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("any JSON value")
    }

    fn visit_map<A: MapAccess<'de>>(self, mut access: A) -> Result<Unambiguous, A::Error> {
        let mut object = serde_json::Map::new();

        while let Some(name) = access.next_key::<String>()? {
            // Recursing through the same wrapper is what makes this hold inside
            // nested objects and arrays, not only at the top level.
            let Unambiguous(member) = access.next_value()?;

            if object.insert(name, member).is_some() {
                return Err(serde::de::Error::custom(AMBIGUOUS_NAME));
            }
        }

        Ok(Unambiguous(Value::Object(object)))
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut access: A) -> Result<Unambiguous, A::Error> {
        let mut items = Vec::new();

        while let Some(Unambiguous(item)) = access.next_element()? {
            items.push(item);
        }

        Ok(Unambiguous(Value::Array(items)))
    }

    fn visit_bool<E>(self, value: bool) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::Bool(value)))
    }

    fn visit_i64<E>(self, value: i64) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::from(value)))
    }

    fn visit_u64<E>(self, value: u64) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::from(value)))
    }

    fn visit_f64<E>(self, value: f64) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::from(value)))
    }

    fn visit_str<E>(self, value: &str) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::String(value.to_owned())))
    }

    fn visit_string<E>(self, value: String) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::String(value)))
    }

    fn visit_unit<E>(self) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::Null))
    }

    fn visit_none<E>(self) -> Result<Unambiguous, E> {
        Ok(Unambiguous(Value::Null))
    }

    fn visit_some<D: Deserializer<'de>>(self, deserializer: D) -> Result<Unambiguous, D::Error> {
        Deserialize::deserialize(deserializer)
    }
}

/// The RFC 6901 pointers of every member holding a replacement character.
///
/// A member whose stored value really contains `EF BF BD` -- a U+FFFD the
/// operator put there -- looks exactly like a replaced one and is named too.
/// This only runs once the body as a whole is not valid UTF-8, so it overnames
/// within a document already known to be affected rather than raising a false
/// alarm on a clean one.
fn replaced_members(value: &Value) -> Vec<String> {
    let mut found = Vec::new();
    walk(value, String::new(), &mut found);
    // A member whose name and value were both replaced pushes the same pointer
    // twice, and the two pushes are adjacent.
    found.dedup();
    found
}

fn walk(value: &Value, pointer: String, found: &mut Vec<String>) {
    match value {
        Value::String(string) => {
            if string.contains(REPLACEMENT) {
                found.push(pointer);
            }
        }
        Value::Array(items) => {
            for (index, item) in items.iter().enumerate() {
                walk(item, format!("{}/{}", pointer, index), found);
            }
        }
        Value::Object(members) => {
            for (name, member) in members {
                let child = format!("{}/{}", pointer, escape(name));
                if name.contains(REPLACEMENT) {
                    found.push(child.clone());
                }
                walk(member, child, found);
            }
        }
        _ => {}
    }
}

/// RFC 6901 escaping for a member name used as one pointer segment.
fn escape(name: &str) -> String {
    name.replace('~', "~0").replace('/', "~1")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    /// A configuration Unit will happily store and hand back: the value of
    /// /routes/0/match/uri holds a raw 0xFF, which no UTF-8 decoder accepts.
    fn body_with_raw_byte() -> Vec<u8> {
        let mut bytes = br#"{"routes":[{"match":{"uri":"/caf"#.to_vec();
        bytes.extend_from_slice(&[0xc3, 0xa9, b'-', 0xff, b'-', b'r', b'a', b'w']);
        bytes.extend_from_slice(br#""},"action":{"return":204}}]}"#);
        bytes
    }

    #[test]
    fn valid_utf8_is_decoded_exactly() {
        let (value, fidelity) = decode::<Value>(r#"{"a":"café"}"#.as_bytes()).expect("valid body must decode");
        assert_eq!(value, serde_json::json!({"a": "café"}));
        assert_eq!(fidelity, Fidelity::Exact);
        assert!(fidelity.warning("/config").is_none());
    }

    #[test]
    fn undecodable_bytes_are_replaced_and_reported() {
        let (value, fidelity) = decode::<Value>(&body_with_raw_byte()).expect("legacy body must still be readable");

        // The document is intact apart from the one byte that cannot be
        // carried by a Rust string.
        assert_eq!(value["routes"][0]["match"]["uri"], "/café-\u{fffd}-raw");
        assert_eq!(value["routes"][0]["action"]["return"], 204);

        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/routes/0/match/uri".to_string()]
            }
        );

        let warning = fidelity.warning("/config").expect("a lossy decode must warn");
        assert!(warning.contains("/config"));
        assert!(warning.contains("/routes/0/match/uri"));
        assert!(warning.contains("not valid UTF-8"));
    }

    /// The typed callers must get the same treatment as `Value` ones: the
    /// error-body path deserializes into `Value`, but a caller asking for a
    /// map or a `String` goes through `from_value`.
    #[test]
    fn a_typed_response_is_decoded_lossily_too() {
        let mut bytes = br#"{"error":"Unknown parameter \""#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#"\"."}"#);

        let (map, fidelity) = decode::<HashMap<String, String>>(&bytes).expect("error body must survive");
        assert_eq!(map["error"], "Unknown parameter \"\u{fffd}\".");
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/error".to_string()]
            }
        );
    }

    /// Two application names that differ only in their undecodable byte decode
    /// to one name, and `serde_json` keeps the last of two equal names: the
    /// first application's whole definition would be gone from what is shown,
    /// under a warning that mentions only replaced bytes.  Fail instead.
    #[test]
    fn names_that_collide_when_decoded_are_refused() {
        let mut bytes = br#"{"applications":{"app-"#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"type":"external","keep":"first"},"app-"#);
        bytes.push(0xfe);
        bytes.extend_from_slice(br#"":{"type":"python","keep":"second"}}}"#);

        // What the replacing decode on its own produces, and why it cannot be
        // shown: one application, and it is not the first one.
        let lossy: Value = serde_json::from_str(&String::from_utf8_lossy(&bytes)).expect("the lossy view parses");
        assert_eq!(lossy["applications"].as_object().expect("an object").len(), 1);

        let error = decode::<Value>(&bytes).expect_err("a decode that drops a member must fail");
        let message = error.to_string();
        assert!(
            message.contains("differ only in bytes that are not valid UTF-8"),
            "{}",
            message
        );
        assert!(message.contains("incomplete"), "{}", message);
    }

    /// The collision check must not cost the bodies it is not about: one bad
    /// byte in each of two *different* objects collides with nothing.
    #[test]
    fn replaced_names_in_different_objects_are_not_a_collision() {
        let mut bytes = br#"{"applications":{"app-"#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"type":"external"}},"upstreams":{"up-"#);
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"servers":{}}}}"#);

        let (value, fidelity) = decode::<Value>(&bytes).expect("two separate objects must still decode");
        assert!(value["applications"]["app-\u{fffd}"].is_object());
        assert!(value["upstreams"]["up-\u{fffd}"].is_object());
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec![
                    "/applications/app-\u{fffd}".to_string(),
                    "/upstreams/up-\u{fffd}".to_string()
                ]
            }
        );
    }

    /// Two equal *values* are not a collision either: only names are keyed.
    #[test]
    fn replaced_values_are_never_a_collision() {
        let mut bytes = br#"{"routes":[{"match":{"uri":"/"#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#""}},{"match":{"uri":"/"#);
        bytes.push(0xfe);
        bytes.extend_from_slice(br#""}}]}"#);

        let (value, _) = decode::<Value>(&bytes).expect("equal values are not equal names");
        assert_eq!(value["routes"].as_array().expect("an array").len(), 2);
    }

    /// A body that will not parse as JSON is shown, and a body shown on a
    /// terminal is neither unbounded nor trusted.
    #[test]
    fn an_unparseable_body_is_bounded_and_rendered() {
        // A proxy's error page, in the shape that matters: big.
        let huge = body_for_display("A".repeat(5 * 1024 * 1024).as_bytes());
        assert!(
            huge.len() < MAX_SHOWN_BODY + 128,
            "the body must be bounded, got {} bytes",
            huge.len()
        );
        assert!(huge.starts_with("AAA"));
        // The note counts what was measured, which is the output.
        assert!(huge.ends_with("[... truncated after 2048 bytes of output]"), "{}", huge);

        // An escape sequence in that page must not reach the terminal, and is
        // written out as the escape rather than as U+FFFD, which here would
        // claim a byte could not be decoded.  Newlines and tabs stay: a page is
        // many lines.
        let hostile = body_for_display(b"<h1>502\x1b[2J\x1b]0;pwned\x07</h1>\n\tat proxy\r\n");
        assert_eq!(
            hostile,
            "<h1>502\\u001b[2J\\u001b]0;pwned\\u0007</h1>\n\tat proxy\\u000d\n"
        );

        // Bytes that are not UTF-8 are shown as U+FFFD, which is what U+FFFD
        // means here: a byte that could not be decoded.
        assert_eq!(body_for_display(&[b'o', b'k', 0xff]), "ok\u{fffd}");

        // A body that needs neither is passed through unchanged.
        assert_eq!(body_for_display(b"<html>502</html>"), "<html>502</html>");
    }

    /// Reading no more of the body than can be shown is the point of the bound,
    /// so the rendering must not depend on how much came after it.
    #[test]
    fn only_as_much_of_a_body_as_can_be_shown_is_read() {
        let mut page = b"<h1>502</h1>".to_vec();
        page.resize(8 * 1024 * 1024, b'x');

        let shown = body_for_display(&page);
        assert!(shown.starts_with("<h1>502</h1>xxx"));
        assert!(shown.len() < MAX_SHOWN_BODY + 128);

        // A character the cut lands inside must not break the rendering.
        let mut multibyte = "caf\u{e9}".repeat(2000).into_bytes();
        multibyte.truncate(MAX_SHOWN_BODY + 1);
        let shown = body_for_display(&multibyte);
        assert!(shown.starts_with("caf\u{e9}caf\u{e9}"), "{}", &shown[..16]);
        assert!(shown.len() < MAX_SHOWN_BODY + 128);
    }

    /// A member name can hold any character JSON can spell, including the
    /// escape for ESC, and every warning interpolates the pointer into stderr.
    /// An application named with an ANSI sequence must not be able to clear the
    /// operator's screen, and one named with a newline must not be able to forge
    /// a line that looks like unitctl's own.
    ///
    /// The rendering is asserted, not merely the absence of control characters:
    /// an absence passes for U+FFFD too, and U+FFFD here would claim the name
    /// holds a byte the server could not hand over, which it does not.
    #[test]
    fn a_pointer_cannot_carry_control_characters_to_the_terminal() {
        // The name is the six characters backslash-u-0-0-1-b and so on: what a
        // server would send, not a raw control byte, which the parser refuses.
        let mut bytes = Vec::new();
        bytes.extend_from_slice(b"{\"app");
        bytes.extend_from_slice(b"\\u001b[2J");
        bytes.extend_from_slice(b"\\u0007");
        bytes.extend_from_slice(b"\\nWarning: all is well");
        bytes.extend_from_slice(b"\\r\\t\":\"caf");
        bytes.push(0xff);
        bytes.extend_from_slice(b"\"}");

        // serde_json really does decode those into control characters: without
        // that this test would be proving nothing.
        let decoded: Value = serde_json::from_str(&String::from_utf8_lossy(&bytes)).expect("the body parses");
        let name = decoded.as_object().expect("an object").keys().next().expect("a name");
        assert!(name.contains('\u{1b}'), "serde must decode the escape: {:?}", name);
        assert!(name.contains('\n'));

        let (_, fidelity) = decode::<Value>(&bytes).expect("the body must still be readable");

        // Every control character is written out, as the escape and not as
        // U+FFFD, and the ordinary text around them still identifies the member.
        let listed = fidelity.members();
        assert_eq!(listed, "/app\\u001b[2J\\u0007\\u000aWarning: all is well\\u000d\\u0009");
        assert!(
            !listed.contains(REPLACEMENT),
            "a name that is valid UTF-8 must not be shown as a replaced byte: {:?}",
            listed
        );

        let warning = fidelity.warning("/config").expect("a lossy decode must warn");
        assert!(warning.ends_with(&listed), "{}", warning);
        // The warning's own two lines are the only newlines in it.
        assert_eq!(warning.matches('\n').count(), 1, "a newline survived: {:?}", warning);
    }

    /// The percent sign was once how this check told undecodable bytes apart,
    /// and JSON can spell a percent as an escape, so neither a literal `%FF` in
    /// a name nor an escaped one may hide a collision.  Asking the question of
    /// the decoded names is what makes the spelling irrelevant.
    #[test]
    fn no_spelling_of_a_name_hides_a_collision() {
        let mut literal = br#"{"applications":{"app-"#.to_vec();
        literal.push(0xff);
        literal.extend_from_slice(br#"":{"keep":"raw-ff"},"app-"#);
        literal.push(0xfe);
        literal.extend_from_slice(br#"":{"keep":"raw-fe"},"app-%FF":{"keep":"literal"}}}"#);

        let mut escaped = br#"{"applications":{"app-"#.to_vec();
        escaped.push(0xff);
        escaped.extend_from_slice(br#"":{"keep":"raw-ff"},"app-"#);
        escaped.push(0xfe);
        escaped.extend_from_slice(br#"":{"keep":"raw-fe"},"app-"#);
        // An escaped percent: the six characters backslash-u-0-0-2-5.
        escaped.extend_from_slice(b"\\u0025");
        escaped.extend_from_slice(br#"FF":{"keep":"escaped"}}}"#);

        for bytes in [&literal, &escaped] {
            // Three names went in and the replacing decode keeps two, so the
            // body really is one a decode would silently truncate.
            let lossy: Value = serde_json::from_str(&String::from_utf8_lossy(bytes)).expect("the lossy view parses");
            assert_eq!(lossy["applications"].as_object().expect("an object").len(), 2);

            let error = decode::<Value>(bytes).expect_err("a decode that drops a member must fail");
            assert!(
                error
                    .to_string()
                    .contains("differ only in bytes that are not valid UTF-8"),
                "{}",
                error
            );
        }
    }

    /// The one collision where neither name was damaged by the decode: an
    /// operator can legitimately store a name holding a real U+FFFD (`EF BF BD`),
    /// and a sibling holding a raw undecodable byte decodes to the same thing.
    #[test]
    fn a_stored_replacement_character_collides_with_a_replaced_byte() {
        let mut bytes = br#"{"applications":{"app-"#.to_vec();
        bytes.extend_from_slice("\u{fffd}".as_bytes());
        bytes.extend_from_slice(br#"":{"keep":"stored"},"app-"#);
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"keep":"raw"}}}"#);

        // The stored name is valid UTF-8 and arrives untouched; it is the raw
        // byte that decodes onto it.
        let lossy: Value = serde_json::from_str(&String::from_utf8_lossy(&bytes)).expect("the lossy view parses");
        assert_eq!(lossy["applications"].as_object().expect("an object").len(), 1);

        let error = decode::<Value>(&bytes).expect_err("one of the two would be dropped");
        assert!(error.to_string().contains("share a name"), "{}", error);
    }

    /// Duplicate names are refused whatever made them duplicate, so the refusal
    /// must not blame undecodable bytes as though it had checked.
    ///
    /// A body that is valid UTF-8 never reaches the wrapper at all: the strict
    /// decode succeeds first, dropping a member as `serde_json` always has.
    /// Only a body that also holds an undecodable byte somewhere gets here.
    #[test]
    fn the_refusal_does_not_blame_a_cause_it_did_not_check() {
        let clean = br#"{"applications":{"a":1,"a":2},"x":"ok"}"#;
        let (value, fidelity) = decode::<Value>(clean).expect("the strict decode accepts duplicates");
        assert!(fidelity.is_exact(), "a valid-UTF-8 body must not reach the wrapper");
        assert_eq!(value["applications"]["a"], 2);

        let mut dirty = br#"{"applications":{"a":1,"a":2},"x":"caf"#.to_vec();
        dirty.push(0xff);
        dirty.extend_from_slice(br#""}"#);

        let message = decode::<Value>(&dirty)
            .expect_err("a duplicate name is still a dropped member")
            .to_string();
        assert!(
            message.contains("two members of one object share a name"),
            "{}",
            message
        );
        // The mechanism is offered as the usual reason, not asserted as the cause.
        assert!(message.contains("the usual reason is"), "{}", message);
        assert!(
            message.contains("differ only in bytes that are not valid UTF-8"),
            "{}",
            message
        );
        assert!(message.contains("unitctl export"), "{}", message);
    }

    /// The refusal is about names, at every depth, and nothing else: a duplicate
    /// inside a nested object or inside an array element is the same loss.
    #[test]
    fn a_collision_is_caught_at_every_depth() {
        let mut nested = br#"{"routes":{"a":{"b":{"app-"#.to_vec();
        nested.push(0xff);
        nested.extend_from_slice(br#"":1,"app-"#);
        nested.push(0xfe);
        nested.extend_from_slice(br#"":2}}}}"#);
        assert!(decode::<Value>(&nested).is_err(), "a nested collision must be caught");

        let mut in_array = br#"{"routes":[{"app-"#.to_vec();
        in_array.push(0xff);
        in_array.extend_from_slice(br#"":1,"app-"#);
        in_array.push(0xfe);
        in_array.extend_from_slice(br#"":2}]}"#);
        assert!(
            decode::<Value>(&in_array).is_err(),
            "a collision inside an array element must be caught"
        );
    }

    /// A sequence truncated mid-character is one undecodable run, not a reason
    /// to refuse: there is one name, so there is nothing ambiguous about it.
    #[test]
    fn bytes_that_end_mid_character_still_decode() {
        let mut bytes = br#"{"routes":[{"match":{"uri":"/caf"#.to_vec();
        bytes.extend_from_slice(&[0xe2, 0x82]);
        bytes.extend_from_slice(br#""}}]}"#);

        let (value, fidelity) = decode::<Value>(&bytes).expect("one truncated character is not a collision");
        assert_eq!(value["routes"][0]["match"]["uri"], "/caf\u{fffd}");
        assert!(!fidelity.is_exact());
    }

    /// `decode` is generic, and the callers that read a configuration ask for a
    /// map, which is what refuses a 200 carrying `null`, an array or a bare
    /// string.  That has to hold on the lossy path too, since a body with an
    /// undecodable byte reaches `T` through `from_value` instead of directly.
    #[test]
    fn a_configuration_shape_is_required_on_both_paths() {
        type Config = std::collections::BTreeMap<String, Value>;

        let (config, fidelity) =
            decode::<Config>(br#"{"listeners":{"*:8080":{"pass":"routes"}}}"#).expect("an object is a configuration");
        assert_eq!(config.keys().collect::<Vec<_>>(), vec!["listeners"]);
        assert!(fidelity.is_exact());

        for body in [&b"null"[..], b"[]", b"[{\"listeners\":{}}]", b"\"a string\"", b"204"] {
            assert!(
                decode::<Config>(body).is_err(),
                "{} is not a configuration",
                String::from_utf8_lossy(body)
            );
            // And why the type the caller asks for is what does the refusing:
            // this function cannot, because `Value` accepts every shape.
            assert!(decode::<Value>(body).is_ok());
        }

        // The same bodies with an undecodable byte in them take the lossy path,
        // and must be refused there as well.
        let mut array = br#"["caf"#.to_vec();
        array.push(0xff);
        array.extend_from_slice(br#""]"#);
        assert!(std::str::from_utf8(&array).is_err());
        assert!(decode::<Config>(&array).is_err(), "an array is not a configuration");

        let mut string = br#""caf"#.to_vec();
        string.push(0xff);
        string.push(b'"');
        assert!(
            decode::<Config>(&string).is_err(),
            "a bare string is not a configuration"
        );

        // And an object with such a byte still decodes, lossily.
        let mut object = br#"{"caf"#.to_vec();
        object.push(0xff);
        object.extend_from_slice(br#"":{}}"#);
        let (_, fidelity) = decode::<Config>(&object).expect("an object is still a configuration");
        assert!(!fidelity.is_exact());
    }

    /// A member *name* can hold the raw bytes just as a value can, and an
    /// application name is exactly such a member.
    #[test]
    fn a_replaced_member_name_is_pointed_at_once() {
        let mut bytes = br#"{"applications":{"app-"#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"type":"external"}}}"#);

        let (_, fidelity) = decode::<Value>(&bytes).expect("body must survive");
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/applications/app-\u{fffd}".to_string()]
            }
        );
    }

    /// `/` and `~` in a member name are pointer syntax and must be escaped, or
    /// the pointer names a member that does not exist.
    #[test]
    fn pointer_segments_are_escaped() {
        let mut bytes = br#"{"listeners":{"*:8080":{"pass":"routes/a~b/"#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#""}}}"#);

        let (_, fidelity) = decode::<Value>(&bytes).expect("body must survive");
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/listeners/*:8080/pass".to_string()]
            }
        );

        assert_eq!(escape("a/b~c"), "a~1b~0c");
    }

    /// Tolerating undecodable bytes must not turn malformed JSON into a
    /// success: a body that is valid UTF-8 fails exactly as it did before.
    #[test]
    fn malformed_utf8_json_still_fails() {
        assert!(decode::<Value>(br#"{"a":"#).is_err());
        assert!(decode::<Value>(b"not json at all").is_err());
        // Undecodable *and* malformed is still an error.
        assert!(decode::<Value>(&[b'{', 0xff]).is_err());
    }

    /// A body that decodes but does not match the type asked for keeps failing
    /// too, rather than being silently coerced.
    #[test]
    fn a_type_mismatch_is_not_hidden() {
        assert!(decode::<HashMap<String, String>>(br#"{"a":1}"#).is_err());
        let mut bytes = br#"{"a":1,"b":""#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#""}"#);
        assert!(decode::<HashMap<String, String>>(&bytes).is_err());
    }

    /// The defensive arm: a lossy decode that located nothing still has to say
    /// something, rather than printing an empty list.
    #[test]
    fn a_lossy_decode_that_located_nothing_still_says_so() {
        let listed = Fidelity::Replaced { members: Vec::new() }.members();
        assert_eq!(listed, "an unlocatable part of the document");
        assert!(Fidelity::Replaced { members: Vec::new() }
            .warning("/config")
            .expect("a lossy decode must warn")
            .contains("an unlocatable part of the document"));
    }

    #[test]
    fn a_long_list_of_members_is_capped() {
        let members: Vec<String> = (0..12).map(|i| format!("/m{}", i)).collect();
        let listed = Fidelity::Replaced { members }.members();
        assert!(listed.starts_with("/m0, /m1,"));
        assert!(listed.ends_with("and 4 more"));
    }
}
