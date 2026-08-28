use std::collections::HashMap;
use std::error::Error as StdError;
use std::fmt::Debug;
use std::rc::Rc;
use std::{fmt, io};

use bytes::{Buf, Bytes};
use custom_error::custom_error;
use http_body_util::{BodyExt, Full};
use hyper::{http, Request};
use hyper_rustls::{HttpsConnector, HttpsConnectorBuilder};
use hyper_util::client::legacy::connect::HttpConnector;
use hyper_util::client::legacy::{Client, ResponseFuture};
use hyper_util::rt::TokioExecutor;
use hyperlocal::UnixConnector;
use serde::{Deserialize, Serialize};

use crate::control_socket_address::ControlSocket;
use unit_openapi::apis::configuration::Configuration;
use unit_openapi::apis::{
    ApplicationsApi, ApplicationsApiClient, AppsApi, AppsApiClient, Error as OpenAPIError, ListenersApi,
    ListenersApiClient, StatusApi, StatusApiClient,
};
use unit_openapi::models::{ConfigApplication, ConfigListener, Status};

const USER_AGENT: &str = concat!("Unit CLI/", env!("CARGO_PKG_VERSION"), "/rust");

custom_error! {pub UnitClientError
    OpenAPIError { source: OpenAPIError } = "OpenAPI error",
    JsonError { source: serde_json::Error,
                path: String} = "JSON error [path={path}]",
    HyperError { source: hyper_util::client::legacy::Error,
                 control_socket_address: String,
                 path: String} = "Communications error [control_socket_address={control_socket_address}, path={path}]: {source}",
    HyperBodyError { source: hyper::Error,
                     control_socket_address: String,
                     path: String} = "Body read error [control_socket_address={control_socket_address}, path={path}]: {source}",
    HttpRequestError { source: http::Error,
                       path: String} = "HTTP error [path={path}]",
    HttpResponseError { status: http::StatusCode,
                        path: String,
                        body: String} = "HTTP response error [path={path}, status={status}]:\n{body}",
    HttpResponseJsonBodyError { status: http::StatusCode,
                                path: String,
                                error: String,
                                detail: String,
                                location: String,
                                suggestion: String} = "HTTP response error [path={path}, status={status}]:\n  Error: {error}\n  Detail: {detail}{location}{suggestion}",
    IoError { source: io::Error, socket: String } = "IO error [socket={socket}]",
    UnixSocketAddressError {
        source: io::Error,
        control_socket_address: String
    } = "Invalid unix domain socket address [control_socket_address={control_socket_address}]",
    SocketPermissionsError { control_socket_address: String } =
    "Insufficient permissions to connect to control socket [control_socket_address={control_socket_address}]",
    UnixSocketNotFound { control_socket_address: String } = "Unix socket not found [control_socket_address={control_socket_address}]",
    TcpSocketAddressUriError {
        source: http::uri::InvalidUri,
        control_socket_address: String
    } = "Invalid TCP socket address [control_socket_address={control_socket_address}]",
    TcpSocketAddressParseError {
        message: String,
        control_socket_address: String
    } = "Invalid TCP socket address [control_socket_address={control_socket_address}]: {message}",
    TcpSocketAddressNoPortError {
        control_socket_address: String
    } = "TCP socket address does not have a port specified [control_socket_address={control_socket_address}]",
    UnitdProcessParseError {
        message: String,
        pid: u64
    } = "{message} for [pid={pid}]",
    UnitdProcessExecError {
        source: Box<dyn StdError>,
        message: String,
        executable_path: String,
        pid: u64
    } = "{message} for [pid={pid}, executable_path={executable_path}]: {source}",
    UnitdDockerError {
        message: String
    } = "Failed to communicate with docker daemon: {message}",
}

impl UnitClientError {
    fn new(error: hyper_util::client::legacy::Error, control_socket_address: String, path: String) -> Self {
        if error.is_connect() {
            if let Some(source) = error.source() {
                if let Some(io_error) = source.downcast_ref::<io::Error>() {
                    if io_error.kind().eq(&io::ErrorKind::PermissionDenied) {
                        return UnitClientError::SocketPermissionsError { control_socket_address };
                    }
                }
            }
        }

        UnitClientError::HyperError {
            source: error,
            control_socket_address,
            path,
        }
    }
}

macro_rules! new_openapi_client_from_hyper_client {
    ($unit_client:expr, $hyper_client: ident, $api_client:ident, $api_trait:ident) => {{
        let config = Configuration {
            base_path: $unit_client.control_socket.create_uri_with_path("/").to_string(),
            user_agent: Some(format!("{}/OpenAPI-Generator", USER_AGENT).to_owned()),
            client: $hyper_client.clone(),
            basic_auth: None,
            oauth_access_token: None,
            api_key: None,
        };
        let rc_config = Rc::new(config);
        Box::new($api_client::new(rc_config)) as Box<dyn $api_trait>
    }};
}

macro_rules! new_openapi_client {
    ($unit_client:expr, $api_client:ident, $api_trait:ident) => {
        match &*$unit_client.client {
            RemoteClient::Tcp { client } => {
                new_openapi_client_from_hyper_client!($unit_client, client, $api_client, $api_trait)
            }
            RemoteClient::Unix { client } => {
                new_openapi_client_from_hyper_client!($unit_client, client, $api_client, $api_trait)
            }
        }
    };
}

#[derive(Clone)]
pub enum RemoteClient {
    Unix {
        client: Client<UnixConnector, Full<Bytes>>,
    },
    Tcp {
        client: Client<HttpsConnector<HttpConnector>, Full<Bytes>>,
    },
}

impl RemoteClient {
    fn client_name(&self) -> &str {
        match self {
            RemoteClient::Unix { .. } => "Client<UnixConnector, Full<Bytes>>",
            RemoteClient::Tcp { .. } => "Client<HttpsConnector<HttpConnector>, Full<Bytes>>",
        }
    }

    pub fn request(&self, req: Request<Full<Bytes>>) -> ResponseFuture {
        match self {
            RemoteClient::Unix { client } => client.request(req),
            RemoteClient::Tcp { client } => client.request(req),
        }
    }
}

impl Debug for RemoteClient {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.client_name())
    }
}

#[derive(Debug)]
pub struct UnitClient {
    pub control_socket: ControlSocket,
    /// Client for communicating with the control API over the UNIX domain socket
    client: Box<RemoteClient>,
}

/// Pulls the parts of a control API error body out for display: the message, the
/// detail, where the error is, and the name a typo probably meant.  The last two
/// come back pre-formatted, empty when they do not apply, so a response carrying
/// neither renders exactly as it did before they existed.
fn describe_error_body(body: &serde_json::Value) -> (String, String, String, String) {
    let member = |name: &str| {
        body.get(name)
            .and_then(serde_json::Value::as_str)
            .unwrap_or_default()
            .to_string()
    };

    let mut error = member("error");
    if error.is_empty() {
        error = "Unknown error".to_string();
    }

    // A validation error places itself with an RFC 6901 pointer, where the empty
    // string is the document root; an error found while parsing the request has
    // no pointer and places itself by position instead.
    let place = body.get("location").and_then(|location| {
        let number = |name: &str| location.get(name).and_then(serde_json::Value::as_i64);

        match location.get("path").and_then(serde_json::Value::as_str) {
            Some("") => Some("the document root".to_string()),
            Some(pointer) => Some(pointer.to_string()),
            None => match (number("line"), number("column"), number("offset")) {
                (Some(line), Some(column), _) => Some(format!("line {}, column {}", line, column)),
                (_, _, Some(offset)) => Some(format!("byte offset {}", offset)),
                _ => None,
            },
        }
    });

    let location = place.map(|place| format!("\n  Location: {}", place)).unwrap_or_default();

    let suggestion = match member("suggestion").as_str() {
        "" => String::new(),
        name => format!("\n  Did you mean: {}", name),
    };

    (error, member("detail"), location, suggestion)
}

impl UnitClient {
    pub fn new(control_socket: ControlSocket) -> Self {
        if control_socket.is_local_socket() {
            Self::new_unix(control_socket)
        } else {
            Self::new_http(control_socket)
        }
    }

    pub fn new_http(control_socket: ControlSocket) -> Self {
        let connector = HttpsConnectorBuilder::new()
            .with_native_roots()
            .unwrap_or_else(|_| HttpsConnectorBuilder::new().with_webpki_roots())
            .https_or_http()
            .enable_all_versions()
            .build();
        let remote_client: Client<HttpsConnector<HttpConnector>, Full<Bytes>> =
            Client::builder(TokioExecutor::new()).build(connector);
        Self {
            control_socket,
            client: Box::from(RemoteClient::Tcp { client: remote_client }),
        }
    }

    pub fn new_unix(control_socket: ControlSocket) -> UnitClient {
        let remote_client: Client<UnixConnector, Full<Bytes>> =
            Client::builder(TokioExecutor::new()).build(UnixConnector);
        Self {
            control_socket,
            client: Box::from(RemoteClient::Unix { client: remote_client }),
        }
    }

    /// Sends a request to Unit and deserializes the JSON response body into the value of type `RESPONSE`.
    pub async fn send_request_and_deserialize_response<RESPONSE: for<'de> serde::Deserialize<'de>>(
        &self,
        mut request: Request<Full<Bytes>>,
    ) -> Result<RESPONSE, UnitClientError> {
        let uri = request.uri().clone();
        let path: &str = uri.path();

        request.headers_mut().insert("User-Agent", USER_AGENT.parse().unwrap());

        let response = self
            .client
            .request(request)
            .await
            .map_err(|error| UnitClientError::new(error, self.control_socket.to_string(), path.to_string()))?;

        let status = response.status();
        let body_bytes = response
            .into_body()
            .collect()
            .await
            .map_err(|error| UnitClientError::HyperBodyError {
                source: error,
                control_socket_address: self.control_socket.to_string(),
                path: path.to_string(),
            })?
            .to_bytes();

        let mut reader = body_bytes.reader();
        if !status.is_success() {
            // The control API answers an error with "error" and, where they
            // apply, "detail", a "location" object placing the error, and a
            // "suggestion" naming the parameter an unknown one was probably a
            // typo of.  "location" is an object, so the body is not a map of
            // strings and must not be deserialized as one: that fails the
            // whole body and hides the message it carries.
            let body: serde_json::Value =
                serde_json::from_reader(&mut reader).map_err(|error| UnitClientError::JsonError {
                    source: error,
                    path: path.to_string(),
                })?;

            let (error, detail, location, suggestion) = describe_error_body(&body);

            return Err(UnitClientError::HttpResponseJsonBodyError {
                status,
                path: path.to_string(),
                error,
                detail,
                location,
                suggestion,
            });
        }
        serde_json::from_reader(&mut reader).map_err(|error| UnitClientError::JsonError {
            source: error,
            path: path.to_string(),
        })
    }

    pub fn listeners_api(&self) -> Box<dyn ListenersApi + 'static> {
        new_openapi_client!(self, ListenersApiClient, ListenersApi)
    }

    pub async fn listeners(&self) -> Result<HashMap<String, ConfigListener>, Box<UnitClientError>> {
        self.listeners_api().get_listeners().await.or_else(|err| {
            if let OpenAPIError::LegacyClient(client_error) = err {
                Err(Box::new(UnitClientError::new(
                    client_error,
                    self.control_socket.to_string(),
                    "/listeners".to_string(),
                )))
            } else {
                Err(Box::new(UnitClientError::OpenAPIError { source: err }))
            }
        })
    }

    pub fn status_api(&self) -> Box<dyn StatusApi + 'static> {
        new_openapi_client!(self, StatusApiClient, StatusApi)
    }

    pub async fn status(&self) -> Result<Status, Box<UnitClientError>> {
        self.status_api().get_status().await.or_else(|err| {
            if let OpenAPIError::LegacyClient(client_error) = err {
                Err(Box::new(UnitClientError::new(
                    client_error,
                    self.control_socket.to_string(),
                    "/status".to_string(),
                )))
            } else {
                Err(Box::new(UnitClientError::OpenAPIError { source: err }))
            }
        })
    }

    pub fn applications_api(&self) -> Box<dyn ApplicationsApi + 'static> {
        new_openapi_client!(self, ApplicationsApiClient, ApplicationsApi)
    }

    pub async fn applications(&self) -> Result<HashMap<String, ConfigApplication>, Box<UnitClientError>> {
        self.applications_api().get_applications().await.or_else(|err| {
            if let OpenAPIError::LegacyClient(client_error) = err {
                Err(Box::new(UnitClientError::new(
                    client_error,
                    self.control_socket.to_string(),
                    "/applications".to_string(),
                )))
            } else {
                Err(Box::new(UnitClientError::OpenAPIError { source: err }))
            }
        })
    }

    pub async fn per_application_api(&self) -> Box<dyn AppsApi + 'static> {
        new_openapi_client!(self, AppsApiClient, AppsApi)
    }

    pub async fn restart_application(&self, name: &String) -> Result<HashMap<String, String>, Box<UnitClientError>> {
        self.per_application_api()
            .await
            .get_app_restart(name.as_str())
            .await
            .or_else(|err| {
                if let OpenAPIError::LegacyClient(client_error) = err {
                    Err(Box::new(UnitClientError::new(
                        client_error,
                        self.control_socket.to_string(),
                        format!("/control/applications/{}/restart", name),
                    )))
                } else {
                    Err(Box::new(UnitClientError::OpenAPIError { source: err }))
                }
            })
    }

    pub async fn is_running(&self) -> bool {
        self.status().await.is_ok()
    }
}

pub type UnitSerializableMap = HashMap<String, serde_json::Value>;

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct UnitStatus {
    pub connections: UnitStatusConnections,
    pub requests: UnitStatusRequests,
    pub applications: HashMap<String, UnitStatusApplication>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct UnitStatusConnections {
    #[serde(default)]
    pub closed: usize,
    #[serde(default)]
    pub idle: usize,
    #[serde(default)]
    pub active: usize,
    #[serde(default)]
    pub accepted: usize,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct UnitStatusRequests {
    #[serde(default)]
    pub active: usize,
    #[serde(default)]
    pub total: usize,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct UnitStatusApplication {
    #[serde(default)]
    pub processes: HashMap<String, usize>,
    #[serde(default)]
    pub requests: HashMap<String, usize>,
}

#[cfg(test)]
mod tests {
    use crate::unitd_instance::UnitdInstance;

    use super::*;

    /// The shapes nxt_controller_response() can send, and how each renders.
    #[test]
    fn describes_every_error_body_shape() {
        let case = |body: serde_json::Value| {
            let (error, detail, location, suggestion) = describe_error_body(&body);
            format!("{}|{}|{}|{}", error, detail, location, suggestion)
        };

        // A plain error, as before location and suggestion existed: both parts
        // must be empty so the rendered message is unchanged.
        assert_eq!(
            case(serde_json::json!({"error": "Value doesn't exist."})),
            "Value doesn't exist.|||"
        );

        // A validation error: an RFC 6901 pointer at the member at fault.
        assert_eq!(
            case(serde_json::json!({
                "error": "Invalid configuration.",
                "detail": "Unknown parameter \"pas\".",
                "location": {"path": "/routes/0/action"},
                "suggestion": "pass"
            })),
            "Invalid configuration.|Unknown parameter \"pas\".|\n  Location: /routes/0/action|\n  Did you mean: pass"
        );

        // The empty pointer is the document root, not a missing value.
        assert_eq!(
            case(serde_json::json!({"error": "Invalid configuration.",
                                    "location": {"path": ""}})),
            "Invalid configuration.||\n  Location: the document root|"
        );

        // An error found while parsing places itself by position: no pointer.
        assert_eq!(
            case(serde_json::json!({"error": "Invalid JSON.",
                                    "location": {"offset": 12, "line": 2, "column": 5}})),
            "Invalid JSON.||\n  Location: line 2, column 5|"
        );

        // Offset without line/column, which the controller sends when the
        // position is known but not the line.
        assert_eq!(
            case(serde_json::json!({"error": "Invalid JSON.", "location": {"offset": 12}})),
            "Invalid JSON.||\n  Location: byte offset 12|"
        );

        // Nothing usable in location, and a body that is not an object at all:
        // neither may panic or invent a location.
        assert_eq!(
            case(serde_json::json!({"error": "Invalid JSON.", "location": {}})),
            "Invalid JSON.|||"
        );
        assert_eq!(case(serde_json::json!("not an object")), "Unknown error|||");
    }

    // Integration tests

    #[tokio::test]
    async fn can_connect_to_unit_api() {
        match UnitdInstance::running_unitd_instances().await.first() {
            Some(unit_instance) => {
                let control_api_socket_address = unit_instance
                    .control_api_socket_address()
                    .expect("No control API socket path found");
                let control_socket = ControlSocket::try_from(control_api_socket_address)
                    .expect("Unable to parse control socket address");
                let unit_client = UnitClient::new(control_socket);
                assert!(unit_client.is_running().await);
            }
            None => {
                eprintln!("No running unitd instances found - skipping test");
            }
        }
    }

    #[tokio::test]
    async fn can_get_unit_status() {
        match UnitdInstance::running_unitd_instances().await.first() {
            Some(unit_instance) => {
                let control_api_socket_address = unit_instance
                    .control_api_socket_address()
                    .expect("No control API socket path found");
                let control_socket = ControlSocket::try_from(control_api_socket_address)
                    .expect("Unable to parse control socket address");
                let unit_client = UnitClient::new(control_socket);
                let status = unit_client.status().await.expect("Unable to get unit status");
                println!("Unit status: {:?}", status);
            }
            None => {
                eprintln!("No running unitd instances found - skipping test");
            }
        }
    }

    #[tokio::test]
    async fn can_get_unit_listeners() {
        match UnitdInstance::running_unitd_instances().await.first() {
            Some(unit_instance) => {
                let control_api_socket_address = unit_instance
                    .control_api_socket_address()
                    .expect("No control API socket path found");
                let control_socket = ControlSocket::try_from(control_api_socket_address)
                    .expect("Unable to parse control socket address");
                let unit_client = UnitClient::new(control_socket);
                unit_client.listeners().await.expect("Unable to get Unit listeners");
            }
            None => {
                eprintln!("No running unitd instances found - skipping test");
            }
        }
    }
}
