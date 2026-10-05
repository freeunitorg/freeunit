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
    /// this altered, and `names` the ones among them whose *name* was altered
    /// rather than its value -- a pointer in `names` cannot be PUT to, because
    /// the path itself holds U+FFFD where the server holds a byte.
    Replaced { members: Vec<String>, names: Vec<String> },
}

impl Fidelity {
    pub fn is_exact(&self) -> bool {
        matches!(self, Fidelity::Exact)
    }

    /// The affected members, as a comma-separated list of RFC 6901 pointers.
    pub fn members(&self) -> String {
        let members = match self {
            Fidelity::Exact => return String::new(),
            Fidelity::Replaced { members, .. } => members,
        };

        if members.is_empty() {
            // The bytes did not survive as a string value, so there is nothing
            // to point at.  Say that rather than printing an empty list.
            return "an unlocatable part of the document".to_string();
        }

        list(members)
    }

    /// The affected members whose *name* was altered, as a comma-separated list
    /// of RFC 6901 pointers, or empty when none were.
    ///
    /// These are the ones no PUT can repair: the pointer holds U+FFFD where the
    /// server holds a byte, so the path names a member that does not exist.
    pub fn replaced_names(&self) -> String {
        match self {
            Fidelity::Exact => String::new(),
            Fidelity::Replaced { names, .. } => list(names),
        }
    }

    /// The affected members whose *value* was altered, as a comma-separated list
    /// of RFC 6901 pointers, or empty when none were.
    ///
    /// These are the ones a PUT to the path shown does repair.
    pub fn replaced_values(&self) -> String {
        let (members, names) = match self {
            Fidelity::Exact => return String::new(),
            Fidelity::Replaced { members, names } => (members, names),
        };

        let values: Vec<String> = members
            .iter()
            .filter(|member| !names.contains(member))
            .cloned()
            .collect();

        list(&values)
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

/// Pointers as one line for a person to read: capped, and with the control
/// characters a name can hold written out.
fn list(members: &[String]) -> String {
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

            // A second decode, so that a U+FFFD the operator stored is not
            // reported as one this decode wrote.
            let marked = serde_json::from_str::<Marked>(&marked_decode(bytes)).ok();
            let altered = replaced_members(&value, marked.as_ref());
            let typed = serde_json::from_value(value)?;

            Ok((
                typed,
                Fidelity::Replaced {
                    members: altered.all,
                    names: altered.names,
                },
            ))
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

/// What the second decode writes where bytes could not be decoded.
///
/// Nothing ever searches for this text, and nothing about it needs to be
/// special: a string is reported because its two decodings *differ*, not because
/// it contains a marker.  The only requirements are that it is not U+FFFD, so
/// that a byte which could not be decoded reads differently in the two
/// decodings, and that it is plain ASCII, so that decoding cannot produce it.
///
/// Searching for it instead would be forgeable, which is why this does not: a
/// value that is literally this text defeats a search, and so does JSON's own
/// escaping spelling it out -- and checking that the text is absent from the raw
/// bytes does not see the escaped spelling, so that check would not help either.
const MARKER: &str = "?";

/// The same bytes decoded a second time, with every subsequence that is not
/// valid UTF-8 written as [`MARKER`] instead of U+FFFD.
///
/// Grouped as `String::from_utf8_lossy` groups: one replacement per maximal
/// ill-formed subsequence.  So the two decodings of one string agree on how many
/// replacements it has and where they fall, and two names equal under one are
/// equal under the other -- which is what lets the two trees be walked together.
fn marked_decode(bytes: &[u8]) -> String {
    let mut text = String::new();
    let mut rest = bytes;

    loop {
        let error = match std::str::from_utf8(rest) {
            Ok(valid) => {
                text.push_str(valid);
                return text;
            }
            Err(error) => error,
        };

        let (valid, invalid) = rest.split_at(error.valid_up_to());
        text.push_str(std::str::from_utf8(valid).unwrap_or_default());
        text.push_str(MARKER);

        // `error_len()` is None when the bytes end mid-sequence: the rest of the
        // input is that one incomplete sequence.
        let undecodable = error.error_len().unwrap_or(invalid.len());
        rest = &invalid[undecodable..];
    }
}

/// The second decode's tree, keeping every member of every object, in order.
///
/// Deliberately not a `serde_json::Map`.  The marker is ordinary text, so a
/// document can hold a member whose name *is* the marker beside one whose name
/// decodes to it -- a literal `"?"` key and a key holding a byte that cannot be
/// decoded -- and a map would keep one of the two.  The two sides would then
/// disagree on length, the comparison would fall back for that whole object, and
/// every U+FFFD in it would be named: exactly the false positive the comparison
/// exists to prevent, reachable by writing the marker in the document.
///
/// This is the third in-band token this check has had, and the first two were
/// each defeated by content that could contain them.  Keeping the pairs ends
/// that: the marker's text stops mattering to the *structure*, so no document
/// can make the two sides fail to line up.
#[derive(Debug)]
enum Marked {
    Object(Vec<(String, Marked)>),
    Array(Vec<Marked>),
    String(String),
    /// A number, a boolean or null: nothing that can hold a replaced byte.
    Scalar,
}

impl<'de> Deserialize<'de> for Marked {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_any(MarkedVisitor)
    }
}

struct MarkedVisitor;

impl<'de> Visitor<'de> for MarkedVisitor {
    type Value = Marked;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("any JSON value")
    }

    fn visit_map<A: MapAccess<'de>>(self, mut access: A) -> Result<Marked, A::Error> {
        let mut members = Vec::new();

        while let Some(name) = access.next_key::<String>()? {
            members.push((name, access.next_value()?));
        }

        Ok(Marked::Object(members))
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut access: A) -> Result<Marked, A::Error> {
        let mut items = Vec::new();

        while let Some(item) = access.next_element()? {
            items.push(item);
        }

        Ok(Marked::Array(items))
    }

    fn visit_str<E>(self, value: &str) -> Result<Marked, E> {
        Ok(Marked::String(value.to_owned()))
    }

    fn visit_string<E>(self, value: String) -> Result<Marked, E> {
        Ok(Marked::String(value))
    }

    fn visit_bool<E>(self, _value: bool) -> Result<Marked, E> {
        Ok(Marked::Scalar)
    }

    fn visit_i64<E>(self, _value: i64) -> Result<Marked, E> {
        Ok(Marked::Scalar)
    }

    fn visit_u64<E>(self, _value: u64) -> Result<Marked, E> {
        Ok(Marked::Scalar)
    }

    fn visit_f64<E>(self, _value: f64) -> Result<Marked, E> {
        Ok(Marked::Scalar)
    }

    fn visit_unit<E>(self) -> Result<Marked, E> {
        Ok(Marked::Scalar)
    }

    fn visit_none<E>(self) -> Result<Marked, E> {
        Ok(Marked::Scalar)
    }

    fn visit_some<D: Deserializer<'de>>(self, deserializer: D) -> Result<Marked, D::Error> {
        Deserialize::deserialize(deserializer)
    }
}

/// Where a replacing decode altered the document.
///
/// `values` and `names` are RFC 6901 pointers.  They are kept apart because the
/// advice differs: a pointer whose *value* was altered can be PUT to, and one
/// whose *name* was altered cannot -- the path itself holds U+FFFD where the
/// server holds a byte, so a PUT would address a different member.
#[derive(Debug, Default)]
struct Altered {
    all: Vec<String>,
    names: Vec<String>,
}

/// The members this decode actually altered, and which of them are member names.
///
/// Not every U+FFFD in the decoded tree is one of ours.  `EF BF BD` is valid
/// UTF-8, so a U+FFFD an operator stored arrives intact -- and naming it would
/// send them to `edit`'s recovery advice for a member that was never damaged,
/// while the point of this warning is to say where the damage is.
///
/// A string is affected exactly when its two decodings differ: a stored U+FFFD
/// reads the same both times, and a byte that could not be decoded reads U+FFFD
/// in one and [`MARKER`] in the other.  Member names are compared the same way.
/// The pointers are built from the lossy tree, so they read as what the operator
/// will see everywhere else.
///
/// `marked` is `None` -- and every U+FFFD is named, as before -- when the second
/// decode will not parse, and the walk falls back the same way for any subtree
/// whose two sides do not line up.  A comparison that could not be made is not
/// grounds for saying less.  With [`Marked`] keeping duplicate names, objects
/// always line up; what remains is defence, not a path a document can steer into.
fn replaced_members(lossy: &Value, marked: Option<&Marked>) -> Altered {
    let mut found = Altered::default();
    walk(lossy, marked, String::new(), &mut found);
    // A member whose name and value were both replaced pushes the same pointer
    // twice, and the two pushes are adjacent.
    found.all.dedup();
    found.names.dedup();
    found
}

fn walk(lossy: &Value, marked: Option<&Marked>, pointer: String, found: &mut Altered) {
    match lossy {
        Value::String(string) => {
            let other = match marked {
                Some(Marked::String(other)) => Some(other.as_str()),
                _ => None,
            };

            if differs(string, other) {
                found.all.push(pointer);
            }
        }
        Value::Array(items) => {
            let others = match marked {
                Some(Marked::Array(others)) if others.len() == items.len() => Some(others),
                _ => None,
            };

            for (index, item) in items.iter().enumerate() {
                let other = others.map(|others| &others[index]);
                walk(item, other, format!("{}/{}", pointer, index), found);
            }
        }
        Value::Object(members) => {
            // Both decodes keep the server's member order, and `Unambiguous` has
            // already refused any document whose names collide under the lossy
            // decode, so the two objects line up by position.  By position and
            // not by name, because the names are what differ.
            let others = match marked {
                Some(Marked::Object(others)) if others.len() == members.len() => Some(others),
                _ => None,
            };
            let mut others = others.map(|others| others.iter());

            for (name, member) in members {
                let other = others.as_mut().and_then(|others| others.next());
                let child = format!("{}/{}", pointer, escape(name));

                if differs(name, other.map(|(other_name, _)| other_name.as_str())) {
                    found.all.push(child.clone());
                    found.names.push(child.clone());
                }

                walk(member, other.map(|(_, value)| value), child, found);
            }
        }
        _ => {}
    }
}

/// Whether a decoded string holds a byte this decode replaced: it differs from
/// the same string decoded the other way, or -- with nothing to compare against
/// -- it holds a U+FFFD, which is what was reported before the comparison
/// existed.
fn differs(lossy: &str, marked: Option<&str>) -> bool {
    match marked {
        Some(marked) => lossy != marked,
        None => lossy.contains(REPLACEMENT),
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
                members: vec!["/routes/0/match/uri".to_string()],
                names: Vec::new(),
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
                members: vec!["/error".to_string()],
                names: Vec::new(),
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
                ],
                names: vec![
                    "/applications/app-\u{fffd}".to_string(),
                    "/upstreams/up-\u{fffd}".to_string()
                ],
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

    /// `EF BF BD` is valid UTF-8, so a U+FFFD an operator stored arrives intact
    /// and was never replaced.  Naming it sends them to `edit`'s recovery advice
    /// for a member that is not damaged, and the point of the warning is to say
    /// which member is.
    #[test]
    fn a_stored_replacement_character_is_not_reported_as_replaced() {
        let mut bytes = br#"{"valid":"stored: "#.to_vec();
        bytes.extend_from_slice(&[0xef, 0xbf, 0xbd]);
        bytes.extend_from_slice(br#"","legacy":"raw: "#);
        bytes.push(0xff);
        bytes.extend_from_slice(br#""}"#);

        let (value, fidelity) = decode::<Value>(&bytes).expect("the body must still be readable");

        // Both values read as U+FFFD, which is exactly why they cannot be told
        // apart by looking at the decoded text.
        assert_eq!(value["valid"], "stored: \u{fffd}");
        assert_eq!(value["legacy"], "raw: \u{fffd}");

        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/legacy".to_string()],
                names: Vec::new(),
            }
        );

        let warning = fidelity.warning("/config").expect("a lossy decode must warn");
        assert!(warning.contains("/legacy"), "{}", warning);
        assert!(
            !warning.contains("/valid"),
            "an intact member must not be named: {}",
            warning
        );
    }

    /// The same distinction for a member *name*: one name holds a stored U+FFFD
    /// and its sibling holds a byte that could not be decoded.  They are
    /// different names, so nothing collides, and only one was altered.
    #[test]
    fn a_stored_replacement_character_in_a_name_is_not_reported() {
        let mut bytes = br#"{"applications":{"app-"#.to_vec();
        bytes.extend_from_slice(&[0xef, 0xbf, 0xbd]);
        bytes.extend_from_slice(br#"-stored":{"type":"external"},"other-"#);
        bytes.push(0xff);
        bytes.extend_from_slice(br#"-raw":{"type":"python"}}}"#);

        let (_, fidelity) = decode::<Value>(&bytes).expect("two different names do not collide");
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/applications/other-\u{fffd}-raw".to_string()],
                names: vec!["/applications/other-\u{fffd}-raw".to_string()],
            }
        );
    }

    /// With nothing to compare against, every U+FFFD is named, which is what was
    /// reported before the second decode existed.  A comparison that could not be
    /// made is not grounds for saying less.
    #[test]
    fn without_a_second_decode_every_replacement_is_named() {
        let lossy = serde_json::json!({
            "valid": "stored: \u{fffd}",
            "legacy": "raw: \u{fffd}",
            "clean": "nothing here",
        });

        let altered = replaced_members(&lossy, None);
        assert_eq!(altered.all, vec!["/valid".to_string(), "/legacy".to_string()]);
        assert!(altered.names.is_empty(), "no member *name* holds one here");
    }

    /// Nothing searches the second decode for its marker, so content that is
    /// the marker -- or JSON's escaping spelling it out, which no check of the
    /// raw bytes would see -- cannot make an intact member look replaced.
    #[test]
    fn content_that_looks_like_the_marker_is_not_reported() {
        let mut literal = br#"{"innocent":"?","legacy":"raw: "#.to_vec();
        literal.push(0xff);
        literal.extend_from_slice(br#""}"#);

        let mut escaped = br#"{"innocent":""#.to_vec();
        escaped.extend_from_slice(b"\\u003f");
        escaped.extend_from_slice(br#"","legacy":"raw: "#);
        escaped.push(0xff);
        escaped.extend_from_slice(br#""}"#);

        for bytes in [&literal, &escaped] {
            let (value, fidelity) = decode::<Value>(bytes).expect("the body must decode");
            assert_eq!(value["innocent"], "?", "the marker text decodes to itself");
            assert_eq!(
                fidelity,
                Fidelity::Replaced {
                    members: vec!["/legacy".to_string()],
                    names: Vec::new(),
                },
                "only the member with an undecodable byte is named"
            );
        }
    }

    /// The marker is ordinary text, so a document can hold a member whose name
    /// *is* it beside one whose name decodes to it.  Keeping duplicate names on
    /// the marked side is what stops that from collapsing the two sides into
    /// different lengths and falling back to naming every U+FFFD in the object.
    #[test]
    fn a_name_that_is_the_marker_does_not_disturb_the_comparison() {
        let mut bytes = br#"{"applications":{"?":{"note":"literal question mark"},""#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"note":"raw byte"},"valid":{"note":"stored: "#);
        bytes.extend_from_slice(&[0xef, 0xbf, 0xbd]);
        bytes.extend_from_slice(br#""}}}"#);

        // Three names go in, and two of them read as "?" once the bytes that
        // could not be decoded are written as the marker.
        let lossy: Value = serde_json::from_str(&String::from_utf8_lossy(&bytes)).expect("the lossy view parses");
        assert_eq!(lossy["applications"].as_object().expect("an object").len(), 3);

        let (_, fidelity) = decode::<Value>(&bytes).expect("the body must decode");
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/applications/\u{fffd}".to_string()],
                names: vec!["/applications/\u{fffd}".to_string()],
            },
            "only the name holding a byte that could not be decoded was altered"
        );

        // The two members that were not touched: the one literally named "?",
        // and the one whose value holds a U+FFFD the operator stored.
        let warning = fidelity.warning("/config").expect("a lossy decode must warn");
        assert!(!warning.contains("/valid"), "an intact member was named: {}", warning);
    }

    /// A pointer whose *name* was altered cannot be PUT to: it shows U+FFFD
    /// where the server holds a byte, so it names a member that does not exist.
    /// The two kinds are kept apart so the advice can say so.
    #[test]
    fn a_replaced_name_is_reported_apart_from_a_replaced_value() {
        let mut in_a_name = br#"{"applications":{"app-"#.to_vec();
        in_a_name.push(0xff);
        in_a_name.extend_from_slice(br#"":{"type":"external"}}}"#);

        let (_, fidelity) = decode::<Value>(&in_a_name).expect("the body must decode");
        assert_eq!(fidelity.members(), "/applications/app-\u{fffd}");
        assert_eq!(
            fidelity.replaced_names(),
            "/applications/app-\u{fffd}",
            "a replaced name must be reported as one"
        );

        let mut in_a_value = br#"{"routes":[{"match":{"uri":"/caf"#.to_vec();
        in_a_value.push(0xff);
        in_a_value.extend_from_slice(br#""}}]}"#);

        let (_, fidelity) = decode::<Value>(&in_a_value).expect("the body must decode");
        assert_eq!(fidelity.members(), "/routes/0/match/uri");
        assert!(
            fidelity.replaced_names().is_empty(),
            "a replaced value is repairable by a PUT and must not be listed as a name"
        );

        // An exact body has neither.
        let (_, fidelity) = decode::<Value>(br#"{"routes":[]}"#).expect("valid UTF-8 decodes exactly");
        assert!(fidelity.replaced_names().is_empty());
    }

    /// The two lists are what the advice branches on, so each must hold only its
    /// own kind.
    #[test]
    fn values_and_names_are_listed_apart() {
        let mut bytes = br#"{"applications":{"app-"#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"type":"external"}},"routes":[{"uri":"/caf"#);
        bytes.push(0xfe);
        bytes.extend_from_slice(br#""}]}"#);

        let (_, fidelity) = decode::<Value>(&bytes).expect("the body decodes");
        assert_eq!(fidelity.replaced_values(), "/routes/0/uri");
        assert_eq!(fidelity.replaced_names(), "/applications/app-\u{fffd}");
        assert_eq!(
            fidelity.members(),
            "/applications/app-\u{fffd}, /routes/0/uri",
            "the headline still lists both"
        );

        // A body with only a replaced value has no names, and the reverse.
        let mut value_only = br#"{"routes":[{"uri":"/caf"#.to_vec();
        value_only.push(0xff);
        value_only.extend_from_slice(br#""}]}"#);
        let (_, fidelity) = decode::<Value>(&value_only).expect("the body decodes");
        assert_eq!(fidelity.replaced_values(), "/routes/0/uri");
        assert!(fidelity.replaced_names().is_empty());

        // And an exact body has neither.
        let (_, fidelity) = decode::<Value>(br#"{"routes":[]}"#).expect("valid UTF-8 decodes exactly");
        assert!(fidelity.replaced_values().is_empty());
        assert!(fidelity.replaced_names().is_empty());
    }

    /// Both at once: one member's name was altered and another's value was, and
    /// each is reported in its own list.
    #[test]
    fn a_name_and_a_value_are_reported_separately() {
        let mut bytes = br#"{"applications":{"app-"#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#"":{"type":"external"}},"routes":[{"uri":"/caf"#);
        bytes.push(0xfe);
        bytes.extend_from_slice(br#""}]}"#);

        let (_, fidelity) = decode::<Value>(&bytes).expect("the body must decode");
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/applications/app-\u{fffd}".to_string(), "/routes/0/uri".to_string()],
                names: vec!["/applications/app-\u{fffd}".to_string()],
            }
        );
    }

    /// serde_json must keep the order the server sent.  The comparison in
    /// `replaced_members` pairs the two decodes member by member, in order, so a
    /// sorted map would pair them wrongly.  The names here are not in sorted
    /// order, so this fails if the preserve_order feature is ever turned off.
    #[test]
    fn member_order_survives_a_round_trip() {
        let source = r#"{"z":1,"a":2,"m":3}"#;
        let parsed: Value = serde_json::from_str(source).expect("the body parses");

        let names: Vec<&str> = parsed
            .as_object()
            .expect("an object")
            .keys()
            .map(String::as_str)
            .collect();
        assert_eq!(names, vec!["z", "a", "m"], "serde_json must keep member order");

        assert_eq!(serde_json::to_string(&parsed).expect("it serializes"), source);
    }

    /// The same precondition, at the place that depends on it: the two decodes
    /// are paired by position.  With a sorted map the damaged value and the
    /// undamaged one swap places, and both are reported.
    #[test]
    fn members_out_of_sorted_order_are_paired_correctly() {
        let mut bytes = br#"{"z":"ok","a":"raw: "#.to_vec();
        bytes.push(0xff);
        bytes.extend_from_slice(br#""}"#);

        let (_, fidelity) = decode::<Value>(&bytes).expect("the body decodes");
        assert_eq!(
            fidelity,
            Fidelity::Replaced {
                members: vec!["/a".to_string()],
                names: Vec::new(),
            },
            "only /a holds a byte that could not be decoded, and it is a value"
        );
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
                members: vec!["/applications/app-\u{fffd}".to_string()],
                names: vec!["/applications/app-\u{fffd}".to_string()],
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
                members: vec!["/listeners/*:8080/pass".to_string()],
                names: Vec::new(),
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
        let listed = Fidelity::Replaced {
            members: Vec::new(),
            names: Vec::new(),
        }
        .members();
        assert_eq!(listed, "an unlocatable part of the document");
        assert!(Fidelity::Replaced {
            members: Vec::new(),
            names: Vec::new()
        }
        .warning("/config")
        .expect("a lossy decode must warn")
        .contains("an unlocatable part of the document"));
    }

    #[test]
    fn a_long_list_of_members_is_capped() {
        let members: Vec<String> = (0..12).map(|i| format!("/m{}", i)).collect();
        let listed = Fidelity::Replaced {
            members,
            names: Vec::new(),
        }
        .members();
        assert!(listed.starts_with("/m0, /m1,"));
        assert!(listed.ends_with("and 4 more"));
    }
}
