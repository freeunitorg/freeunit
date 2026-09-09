use crate::requests::send_empty_body_read_bytes;
use crate::unitctl::UnitCtl;
use crate::unitctl_error::ControlSocketErrorKind;
use crate::wait;
use crate::UnitctlError;
use std::fs::File;
use std::io::stdout;
use tar::{Builder, Header};
use unit_client_rs::json_body;
use unit_client_rs::unit_client::{UnitClient, UnitSerializableMap};

/// Pretty-print the saved document, and keep the server's own bytes when a Rust
/// string cannot carry them.
///
/// A configuration can hold bytes that are not valid UTF-8, and re-encoding
/// those through `serde_json` would replace them.  A backup that differs from
/// the configuration it backs up is worse than an unformatted one.
///
/// Whichever way the body is read, it has to be a configuration to be stored as
/// one, and `UnitSerializableMap` is what says so: a configuration is a JSON
/// object, and `null`, an array or a bare string answered with a success status
/// is an intermediary talking, not a configuration.  Neither is a truncated
/// answer, and either can arrive with bytes that are not valid UTF-8 -- a
/// Latin-1 error page does -- or without, so both arms ask the same question.
/// Only storing differs: bytes a Rust string can carry are pretty-printed, and
/// bytes it cannot are written exactly as they arrived.
fn prettify_or_keep(raw: &[u8], path: &str) -> Result<Vec<u8>, UnitctlError> {
    // Show the body along with the parser's complaint: "expected value at line 1
    // column 1" says nothing about the captive portal's sign-in page that
    // provoked it, and the page is what tells the operator what happened.
    // json_body::body_for_display bounds it and takes its control characters out.
    let refuse = |error: serde_json::Error| UnitctlError::DeserializationError {
        message: format!("{}: {}\n{}", path, error, json_body::body_for_display(raw)),
    };

    match std::str::from_utf8(raw) {
        Ok(text) => serde_json::from_str::<UnitSerializableMap>(text)
            .and_then(|config| serde_json::to_string_pretty(&config))
            .map(String::into_bytes)
            .map_err(refuse),
        Err(_) => {
            // The lossy view answers whether this is a configuration without
            // being what gets written: the bytes stored below are the server's
            // own.  Replacing a byte cannot turn valid JSON into invalid JSON,
            // nor an object into something else, so a document that passes this
            // way passed before the replacement too.
            serde_json::from_str::<UnitSerializableMap>(&String::from_utf8_lossy(raw)).map_err(refuse)?;

            eprintln!(
                "Warning: {} holds bytes that are not valid UTF-8; saving what the server sent, verbatim",
                path
            );
            eprintln!("Warning: 'unitctl import' cannot replay this file -- restore it with curl");
            Ok(raw.to_vec())
        }
    }
}

pub async fn cmd(cli: &UnitCtl, filename: &String) -> Result<(), UnitctlError> {
    if cli.control_socket_addresses.as_ref().map_or(false, |addrs| addrs.len() > 1) {
        return Err(UnitctlError::ControlSocketError {
            kind: ControlSocketErrorKind::General,
            message: "too many control sockets. specify at most one.".to_string(),
        });
    }

    let mut control_sockets = wait::wait_for_sockets(cli).await?;
    let client = UnitClient::new(control_sockets.pop().unwrap());

    if !filename.ends_with(".tar") {
        eprintln!("Warning: writing uncompressed tarball to {}", filename);
    }

    let current_config = prettify_or_keep(&send_empty_body_read_bytes(&client, "GET", "/config").await?, "/config")?;

    //let current_js_modules = send_empty_body_deserialize_response(&client, "GET", "/js_modules")
    //    .await?;

    let mut conf_header = Header::new_gnu();
    conf_header.set_size(current_config.len() as u64);
    conf_header.set_mode(0o644);
    conf_header.set_cksum();

    // builder has a different type depending on output
    if filename == "-" {
        let mut ar = Builder::new(stdout());
        ar.append_data(&mut conf_header, "config.json", current_config.as_slice())
            .unwrap();
    } else {
        let file = File::create(filename).unwrap();
        let mut ar = Builder::new(file);
        ar.append_data(&mut conf_header, "config.json", current_config.as_slice())
            .unwrap();
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_configuration_is_pretty_printed() {
        let saved = prettify_or_keep(br#"{"listeners":{"*:8080":{"pass":"routes"}}}"#, "/config")
            .expect("a JSON body must be saved");
        let text = String::from_utf8(saved).expect("a UTF-8 body stays UTF-8");
        assert!(text.starts_with("{\n"), "expected pretty output, got {:?}", text);
        assert!(text.contains("\"*:8080\""));
    }

    /// The one case that keeps the server's bytes: no Rust string can carry
    /// them, so re-encoding would change the configuration being backed up.
    #[test]
    fn bytes_that_are_not_utf8_are_kept_verbatim() {
        let mut raw = br#"{"routes":[{"match":{"uri":"/"#.to_vec();
        raw.push(0xff);
        raw.extend_from_slice(br#""},"action":{"return":204}}]}"#);

        let saved = prettify_or_keep(&raw, "/config").expect("a legacy body must still be saved");
        assert_eq!(saved, raw, "the backup must be the bytes the server sent");
    }

    /// A 200 carrying something that is not a configuration must not be written
    /// into the tarball as if it were one.
    #[test]
    fn a_valid_utf8_body_that_is_not_json_fails() {
        let error = prettify_or_keep(b"<html><body><h1>502 Bad Gateway</h1></body></html>", "/config")
            .expect_err("a non-JSON body is not a configuration");
        match error {
            UnitctlError::DeserializationError { message } => {
                assert!(
                    message.contains("/config"),
                    "the message must name the path: {}",
                    message
                )
            }
            other => panic!("expected a deserialization error, got {:?}", other),
        }

        // Truncated, which is the same failure for a different reason.
        assert!(prettify_or_keep(br#"{"listeners":"#, "/config").is_err());
    }

    /// Being unreadable as UTF-8 is not a pass either.  A proxy's Latin-1 error
    /// page fails both tests at once, and it must fail on the one that matters:
    /// it is not a configuration.
    #[test]
    fn a_body_that_is_neither_utf8_nor_json_fails() {
        // "Erreur de passerelle" with the accents written the Latin-1 way, which
        // is not valid UTF-8.
        let mut page = b"<html><body><h1>502 Erreur de passerell".to_vec();
        page.extend_from_slice(&[0xe9, 0xe8]);
        page.extend_from_slice(b"</h1></body></html>");
        assert!(std::str::from_utf8(&page).is_err(), "the page must not be valid UTF-8");

        let error = prettify_or_keep(&page, "/config").expect_err("a Latin-1 error page is not a configuration");
        match error {
            UnitctlError::DeserializationError { message } => {
                assert!(
                    message.contains("/config"),
                    "the message must name the path: {}",
                    message
                )
            }
            other => panic!("expected a deserialization error, got {:?}", other),
        }

        // A document cut short mid-string, holding an undecodable byte as well.
        let mut truncated = br#"{"routes":[{"match":{"uri":"/"#.to_vec();
        truncated.push(0xff);
        assert!(prettify_or_keep(&truncated, "/config").is_err());
    }

    /// Valid JSON is not enough either: a configuration is an object, and a 200
    /// carrying `null` or an array is an intermediary talking.  Archiving one
    /// would write a `config.json` that cannot be a configuration, and `edit`
    /// would offer it for editing and then PUT it back as the whole thing.
    #[test]
    fn a_body_that_is_not_an_object_fails() {
        for body in [&b"null"[..], b"[]", b"[{\"listeners\":{}}]", b"\"a string\"", b"204"] {
            let error = prettify_or_keep(body, "/config").expect_err("only an object is a configuration");
            match error {
                UnitctlError::DeserializationError { message } => assert!(
                    message.contains("/config"),
                    "the message must name the path: {}",
                    message
                ),
                other => panic!("expected a deserialization error, got {:?}", other),
            }
        }
    }

    /// The same question on the other arm: being unreadable as UTF-8 must not
    /// let a non-object through to the tarball.
    #[test]
    fn a_non_object_holding_an_undecodable_byte_fails() {
        let mut array = br#"["caf"#.to_vec();
        array.push(0xff);
        array.extend_from_slice(br#""]"#);
        assert!(std::str::from_utf8(&array).is_err(), "the body must not be valid UTF-8");
        assert!(serde_json::from_str::<serde_json::Value>(&String::from_utf8_lossy(&array)).is_ok());

        assert!(
            prettify_or_keep(&array, "/config").is_err(),
            "an array is not a configuration, whatever its bytes"
        );
    }

    /// The page is what tells the operator what happened; the parser's complaint
    /// about its first character does not.  Both are shown.
    #[test]
    fn a_refusal_shows_the_body_it_refused() {
        let page = b"<html><body>captive portal: sign in at http://portal.example/</body></html>";
        let message = prettify_or_keep(page, "/config")
            .expect_err("not a configuration")
            .to_string();

        assert!(message.contains("/config"), "{}", message);
        assert!(message.contains("expected value"), "the reason stays: {}", message);
        assert!(
            message.contains("captive portal: sign in at http://portal.example/"),
            "the page must be shown: {}",
            message
        );

        // The same holds on the other arm, where the body is not valid UTF-8:
        // a Latin-1 page is shown too, with the undecodable bytes as U+FFFD.
        let mut latin1 = b"<h1>502 Erreur de passerell".to_vec();
        latin1.extend_from_slice(&[0xe9, 0xe8]);
        latin1.extend_from_slice(b"</h1>");
        let message = prettify_or_keep(&latin1, "/config")
            .expect_err("not a configuration")
            .to_string();
        assert!(message.contains("502 Erreur de passerell"), "{}", message);
        assert!(
            message.contains('\u{fffd}'),
            "undecodable bytes are shown as U+FFFD: {}",
            message
        );
    }
}
