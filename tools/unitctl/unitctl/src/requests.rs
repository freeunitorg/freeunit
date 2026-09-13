use super::inputfile::InputFile;
use super::UnitClient;
use super::UnitSerializableMap;
use super::UnitctlError;
use crate::known_size::KnownSize;
use bytes::Bytes;
use http_body_util::Full;
use hyper::Request;
use rustls_pki_types::pem;
use rustls_pki_types::pem::SectionKind;
use std::collections::HashMap;
use std::io::BufRead;
use std::io::Cursor;
use std::io::ErrorKind;
use std::sync::atomic::AtomicUsize;
use unit_client_rs::unit_client::UnitClientError;

/// Send the contents of a file to the unit server
/// We assume that the file is valid and can be sent to the server
pub async fn send_and_validate_config_deserialize_response(
    client: &UnitClient,
    method: &str,
    path: &str,
    input_file: Option<&InputFile>,
) -> Result<UnitSerializableMap, UnitctlError> {
    let body_data = match input_file {
        Some(input) => Some(input.to_unit_serializable_map()?),
        None => None,
    };

    /* Unfortunately, we have load the json text into memory before sending it to the server.
     * This allows for validation of the json content before sending to the server. There may be
     * a better way of doing this and it is worth investigating. */
    let json = serde_json::to_value(&body_data).map_err(|error| UnitClientError::JsonError {
        source: error,
        path: path.into(),
    })?;

    let mime_type = input_file.map(|f| f.mime_type());
    let reader = KnownSize::String(json.to_string());

    streaming_upload_deserialize_response(client, method, path, mime_type, reader)
        .await
        .map_err(|e| UnitctlError::UnitClientError { source: e })
}

/// Send an empty body to the unit server
pub async fn send_empty_body_deserialize_response(
    client: &UnitClient,
    method: &str,
    path: &str,
) -> Result<UnitSerializableMap, UnitctlError> {
    send_body_deserialize_response(client, method, path, None).await
}

/// Send the contents of a PEM file to the unit server
pub async fn send_and_validate_pem_data_deserialize_response(
    client: &UnitClient,
    method: &str,
    path: &str,
    input_file: &InputFile,
) -> Result<UnitSerializableMap, UnitctlError> {
    let bytes: Vec<u8> = input_file.try_into()?;
    {
        let mut cursor = Cursor::new(&bytes);
        validate_pem_items(read_pem_sections(&mut cursor))?;
    }
    let known_size = KnownSize::Vec((*bytes).to_owned());

    streaming_upload_deserialize_response(client, method, path, Some(input_file.mime_type()), known_size)
        .await
        .map_err(|e| UnitctlError::UnitClientError { source: e })
}

/// Name a PEM section for the tally below.
///
/// `None` means the section is not one this check knows, and the caller drops
/// it.  Only the names ending in "Key" or "Certificate" are counted, so the
/// other names are there to keep the section in the tally.
fn section_name(kind: SectionKind) -> Option<&'static str> {
    match kind {
        SectionKind::Certificate => Some("X509Certificate"),
        SectionKind::EcPrivateKey => Some("Sec1Key"),
        SectionKind::Crl => Some("Crl"),
        SectionKind::RsaPrivateKey => Some("Pkcs1Key"),
        SectionKind::PrivateKey => Some("Pkcs8Key"),
        SectionKind::PublicKey => Some("Unknown"),
        SectionKind::Csr => Some("Unknown"),
        // SectionKind is non-exhaustive.  A kind we do not name is dropped, so
        // a file that carries one is judged on its other sections alone.
        _ => None,
    }
}

/// Read every PEM section this check knows about.
///
/// Reading stops at the first error.  The caller reports that error, so
/// anything after it would not be looked at.
fn read_pem_sections(rd: &mut dyn BufRead) -> Vec<Result<SectionKind, UnitctlError>> {
    let mut sections = Vec::new();

    loop {
        match pem::from_buf(rd) {
            Ok(Some((kind, _data))) => {
                if section_name(kind).is_some() {
                    sections.push(Ok(kind));
                }
            }
            Ok(None) => return sections,
            Err(error) => {
                sections.push(Err(pem_error(error)));
                return sections;
            }
        }
    }
}

/// Turn a PEM parse failure into the error unitctl reports.
fn pem_error(error: pem::Error) -> UnitctlError {
    match error {
        pem::Error::Io(source) => UnitctlError::IoError { source },
        other => UnitctlError::IoError {
            source: std::io::Error::new(ErrorKind::InvalidData, other.to_string()),
        },
    }
}

/// Validate the contents of a PEM file
fn validate_pem_items(pem_items: Vec<Result<SectionKind, UnitctlError>>) -> Result<(), UnitctlError> {
    if pem_items.is_empty() {
        let error = UnitctlError::CertificateError {
            message: "No certificates found in file".to_string(),
        };
        return Err(error);
    }

    let mut items_tally: HashMap<String, AtomicUsize> = HashMap::new();

    for pem_item_result in pem_items {
        let pem_item = pem_item_result?;
        let key = section_name(pem_item).unwrap_or("Unknown").to_string();
        if let Some(count) = items_tally.get_mut(key.clone().as_str()) {
            count.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        } else {
            items_tally.insert(key, AtomicUsize::new(1));
        }
    }

    let key_count = items_tally
        .iter()
        .filter(|(key, _)| key.ends_with("Key"))
        .fold(0, |acc, (_, count)| {
            acc + count.load(std::sync::atomic::Ordering::Relaxed)
        });
    let cert_count = items_tally
        .iter()
        .filter(|(key, _)| key.ends_with("Certificate"))
        .fold(0, |acc, (_, count)| {
            acc + count.load(std::sync::atomic::Ordering::Relaxed)
        });

    if key_count == 0 {
        let error = UnitctlError::CertificateError {
            message: "No private keys found in file".to_string(),
        };
        return Err(error);
    }
    if cert_count == 0 {
        let error = UnitctlError::CertificateError {
            message: "No certificates found in file".to_string(),
        };
        return Err(error);
    }

    Ok(())
}

pub async fn send_body_deserialize_response<RESPONSE: for<'de> serde::Deserialize<'de>>(
    client: &UnitClient,
    method: &str,
    path: &str,
    input_file: Option<&InputFile>,
) -> Result<RESPONSE, UnitctlError> {
    match input_file {
        Some(input) => {
            streaming_upload_deserialize_response(client, method, path, Some(input.mime_type()), input.try_into()?)
        }
        None => streaming_upload_deserialize_response(client, method, path, None, KnownSize::Empty),
    }
    .await
    .map_err(|e| UnitctlError::UnitClientError { source: e })
}

/// Read a document without decoding it.
///
/// A caller that will hand the document back to Unit needs the bytes Unit
/// sent: a configuration can hold bytes that no Rust `String` carries, so
/// anything routed through `serde_json` on the way out would return something
/// other than what it was shown.
pub async fn send_empty_body_read_bytes(client: &UnitClient, method: &str, path: &str) -> Result<Bytes, UnitctlError> {
    let request = build_request(client, method, path, None, KnownSize::Empty)
        .map_err(|e| UnitctlError::UnitClientError { source: e })?;
    client
        .send_request_and_collect_body(request)
        .await
        .map_err(|e| UnitctlError::UnitClientError { source: e })
}

async fn streaming_upload_deserialize_response<RESPONSE: for<'de> serde::Deserialize<'de>>(
    client: &UnitClient,
    method: &str,
    path: &str,
    mime_type: Option<String>,
    read: KnownSize,
) -> Result<RESPONSE, UnitClientError> {
    let request = build_request(client, method, path, mime_type, read)?;
    client.send_request_and_deserialize_response(request).await
}

fn build_request(
    client: &UnitClient,
    method: &str,
    path: &str,
    mime_type: Option<String>,
    read: KnownSize,
) -> Result<Request<Full<Bytes>>, UnitClientError> {
    let uri = client.control_socket.create_uri_with_path(path);

    // Materialize the body first so that Content-Length reflects actual bytes produced,
    // not a potentially stale declared estimate; I/O errors are also surfaced here.
    let (body, content_length) = read.into_full_body().map_err(|e| UnitClientError::IoError {
        source: e,
        socket: client.control_socket.to_string(),
    })?;

    let mut request = Request::builder()
        .method(method)
        .header("Content-Length", content_length)
        .uri(uri)
        .body(body)
        .expect("Unable to build request");

    if let Some(content_type) = mime_type {
        request
            .headers_mut()
            .insert("Content-Type", content_type.parse().unwrap());
    }

    Ok(request)
}

#[cfg(test)]
mod tests {
    use super::*;

    const CERT: &str = "-----BEGIN CERTIFICATE-----\nAQID\n-----END CERTIFICATE-----\n";
    const PKCS8_KEY: &str = "-----BEGIN PRIVATE KEY-----\nAQID\n-----END PRIVATE KEY-----\n";
    const RSA_KEY: &str = "-----BEGIN RSA PRIVATE KEY-----\nAQID\n-----END RSA PRIVATE KEY-----\n";
    const EC_KEY: &str = "-----BEGIN EC PRIVATE KEY-----\nAQID\n-----END EC PRIVATE KEY-----\n";
    const PUBLIC_KEY: &str = "-----BEGIN PUBLIC KEY-----\nAQID\n-----END PUBLIC KEY-----\n";
    const ECH_CONFIG: &str = "-----BEGIN ECHCONFIG-----\nAQID\n-----END ECHCONFIG-----\n";

    /// Run the check a PEM upload goes through before it is sent.
    fn validate(pem: &str) -> Result<(), UnitctlError> {
        let mut cursor = Cursor::new(pem.as_bytes());
        validate_pem_items(read_pem_sections(&mut cursor))
    }

    fn error_message(pem: &str) -> String {
        match validate(pem) {
            Err(UnitctlError::CertificateError { message }) => message,
            other => panic!("expected a certificate error, got {:?}", other.is_ok()),
        }
    }

    #[test]
    fn accepts_a_certificate_with_a_key() {
        validate(&format!("{}{}", CERT, PKCS8_KEY)).unwrap();
        validate(&format!("{}{}", CERT, RSA_KEY)).unwrap();
        validate(&format!("{}{}", CERT, EC_KEY)).unwrap();
    }

    #[test]
    fn accepts_a_chain_of_certificates_with_one_key() {
        validate(&format!("{}{}{}", CERT, CERT, PKCS8_KEY)).unwrap();
    }

    #[test]
    fn rejects_a_certificate_with_no_key() {
        assert_eq!(error_message(CERT), "No private keys found in file");
    }

    #[test]
    fn rejects_a_key_with_no_certificate() {
        assert_eq!(error_message(PKCS8_KEY), "No certificates found in file");
    }

    #[test]
    fn rejects_a_file_with_no_pem_section() {
        assert_eq!(error_message(""), "No certificates found in file");
        assert_eq!(error_message("not a pem file\n"), "No certificates found in file");
    }

    /// A public key is not a private key, so it cannot stand in for one.
    #[test]
    fn rejects_a_public_key_as_the_key() {
        assert_eq!(
            error_message(&format!("{}{}", CERT, PUBLIC_KEY)),
            "No private keys found in file"
        );
    }

    /// A section this check does not name is dropped, so the file is judged on
    /// its other sections.
    #[test]
    fn ignores_a_section_it_does_not_name() {
        validate(&format!("{}{}{}", CERT, ECH_CONFIG, PKCS8_KEY)).unwrap();
        assert_eq!(error_message(ECH_CONFIG), "No certificates found in file");
    }

    #[test]
    fn rejects_a_section_with_no_end_marker() {
        let truncated = "-----BEGIN CERTIFICATE-----\nAQID\n";
        match validate(truncated) {
            Err(UnitctlError::IoError { source }) => {
                assert_eq!(source.kind(), ErrorKind::InvalidData);
            }
            other => panic!("expected an IO error, got ok={}", other.is_ok()),
        }
    }
}
