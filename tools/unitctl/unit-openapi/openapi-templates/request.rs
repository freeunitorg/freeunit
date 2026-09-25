use std::collections::HashMap;
use std::pin::Pin;

use base64::engine::general_purpose::NO_PAD;
use base64::engine::GeneralPurpose;
use base64::{alphabet, Engine};

use bytes::Bytes;
use futures::Future;
use http_body_util::{BodyExt, Full};
use hyper::header::{HeaderValue, AUTHORIZATION, CONTENT_LENGTH, CONTENT_TYPE, USER_AGENT};
use hyper_util::client::legacy::connect::Connect;
use serde;
use serde_json;

use super::{configuration, Error};

const MIME_ENCODER: GeneralPurpose = GeneralPurpose::new(&alphabet::STANDARD, NO_PAD);

pub(crate) struct ApiKey {
    pub in_header: bool,
    pub in_query: bool,
    pub param_name: String,
}

impl ApiKey {
    fn key(&self, prefix: &Option<String>, key: &str) -> String {
        match prefix {
            None => key.to_owned(),
            Some(prefix) => format!("{} {}", prefix, key),
        }
    }
}

#[allow(dead_code)]
pub(crate) enum Auth {
    None,
    ApiKey(ApiKey),
    Basic,
    Oauth,
}

pub(crate) struct Request {
    auth: Option<Auth>,
    method: hyper::Method,
    path: String,
    query_params: HashMap<String, String>,
    no_return_type: bool,
    path_params: HashMap<String, String>,
    form_params: HashMap<String, String>,
    header_params: HashMap<String, String>,
    // TODO: multiple body params are possible technically, but not supported here.
    serialized_body: Option<String>,
}

#[allow(dead_code)]
impl Request {
    pub fn new(method: hyper::Method, path: String) -> Self {
        Request {
            auth: None,
            method,
            path,
            query_params: HashMap::new(),
            path_params: HashMap::new(),
            form_params: HashMap::new(),
            header_params: HashMap::new(),
            serialized_body: None,
            no_return_type: false,
        }
    }

    pub fn with_body_param<T: serde::Serialize>(mut self, param: T) -> Self {
        self.serialized_body = Some(serde_json::to_string(&param).unwrap());
        self
    }

    pub fn with_header_param(mut self, basename: String, param: String) -> Self {
        self.header_params.insert(basename, param);
        self
    }

    #[allow(unused)]
    pub fn with_query_param(mut self, basename: String, param: String) -> Self {
        self.query_params.insert(basename, param);
        self
    }

    #[allow(unused)]
    pub fn with_path_param(mut self, basename: String, param: String) -> Self {
        self.path_params.insert(basename, param);
        self
    }

    #[allow(unused)]
    pub fn with_form_param(mut self, basename: String, param: String) -> Self {
        self.form_params.insert(basename, param);
        self
    }

    pub fn returns_nothing(mut self) -> Self {
        self.no_return_type = true;
        self
    }

    pub fn with_auth(mut self, auth: Auth) -> Self {
        self.auth = Some(auth);
        self
    }

    pub fn execute<'a, C, U>(
        self,
        conf: &configuration::Configuration<C>,
    ) -> Pin<Box<dyn Future<Output = Result<U, Error>> + 'a>>
    where
        C: Connect + Clone + std::marker::Send + Sync + 'static,
        U: Sized + std::marker::Send + 'a,
        for<'de> U: serde::Deserialize<'de>,
    {
        let mut query_string = ::url::form_urlencoded::Serializer::new("".to_owned());

        let mut path = self.path;
        for (k, v) in self.path_params {
            path = path.replace(&format!("{{{}}}", k), &v);
        }

        for (key, val) in self.query_params {
            query_string.append_pair(&key, &val);
        }

        let mut uri_str = format!("{}{}", conf.base_path, path);

        let query_string_str = query_string.finish();
        if query_string_str != "" {
            uri_str += "?";
            uri_str += &query_string_str;
        }

        let uri: hyper::Uri = match uri_str.parse() {
            Err(e) => return Box::pin(futures::future::err(Error::UriError(e))),
            Ok(u) => u,
        };

        let mut req_builder = hyper::Request::builder().uri(uri).method(self.method);

        let auth = self.auth.unwrap_or_else(|| {
            if conf.api_key.is_some() {
                panic!("Cannot automatically set the API key from the configuration")
            } else if conf.oauth_access_token.is_some() {
                Auth::Oauth
            } else if conf.basic_auth.is_some() {
                Auth::Basic
            } else {
                Auth::None
            }
        });

        match auth {
            Auth::ApiKey(apikey) => {
                if let Some(ref key) = conf.api_key {
                    let val = apikey.key(&key.prefix, &key.key);
                    if apikey.in_query {
                        query_string.append_pair(&apikey.param_name, &val);
                    }
                    if apikey.in_header {
                        req_builder = req_builder.header(&apikey.param_name, val);
                    }
                }
            }
            Auth::Basic => {
                if let Some(ref auth_conf) = conf.basic_auth {
                    let mut text = auth_conf.0.clone();
                    text.push(':');
                    if let Some(ref pass) = auth_conf.1 {
                        text.push_str(&pass[..]);
                    }
                    let encoded = MIME_ENCODER.encode(&text);
                    req_builder = req_builder.header(AUTHORIZATION, encoded);
                }
            }
            Auth::Oauth => {
                if let Some(ref token) = conf.oauth_access_token {
                    let text = "Bearer ".to_owned() + token;
                    req_builder = req_builder.header(AUTHORIZATION, text);
                }
            }
            Auth::None => {}
        }

        if let Some(ref user_agent) = conf.user_agent {
            req_builder = req_builder.header(
                USER_AGENT,
                match HeaderValue::from_str(user_agent) {
                    Ok(header_value) => header_value,
                    Err(e) => return Box::pin(futures::future::err(super::Error::Header(e))),
                },
            );
        }

        for (k, v) in self.header_params {
            req_builder = req_builder.header(&k, v);
        }

        let req_headers = req_builder.headers_mut().unwrap();
        let body: Full<Bytes> = if self.form_params.len() > 0 {
            req_headers.insert(
                CONTENT_TYPE,
                HeaderValue::from_static("application/x-www-form-urlencoded"),
            );
            let mut enc = ::url::form_urlencoded::Serializer::new("".to_owned());
            for (k, v) in self.form_params {
                enc.append_pair(&k, &v);
            }
            Full::new(Bytes::from(enc.finish()))
        } else if let Some(serialized) = self.serialized_body {
            req_headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
            req_headers.insert(CONTENT_LENGTH, serialized.len().into());
            Full::new(Bytes::from(serialized))
        } else {
            Full::new(Bytes::new())
        };

        let request = match req_builder.body(body) {
            Ok(r) => r,
            Err(e) => return Box::pin(futures::future::err(Error::from(e))),
        };

        let no_return_type = self.no_return_type;
        let client = conf.client.clone();
        Box::pin(async move {
            let response = client.request(request).await.map_err(Error::from)?;

            let status = response.status();
            if !status.is_success() {
                let body_bytes = response
                    .into_body()
                    .collect()
                    .await
                    .map(|b| b.to_bytes())
                    .unwrap_or_default();
                return Err(Error::from((status, body_bytes)));
            }

            if no_return_type {
                return Ok(serde_json::from_str("null").expect("serde null value"));
            }

            let bytes = response
                .into_body()
                .collect()
                .await
                .map_err(Error::from)?
                .to_bytes();
            serde_json::from_slice(&bytes).map_err(Error::from)
        })
    }
}
