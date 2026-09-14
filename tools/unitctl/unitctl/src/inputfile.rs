use std::io;
use std::io::{BufRead, BufReader, Error as IoError, Read};
use std::path::{Path, PathBuf};

use crate::known_size::KnownSize;
use clap::ValueEnum;

use super::UnitSerializableMap;
use super::UnitctlError;

/// Input file data format
#[derive(ValueEnum, Copy, Clone, Debug, PartialEq, Eq)]
pub enum InputFormat {
    /// Recognised, but not parsed.  unitctl no longer reads YAML.  The format
    /// stays in this list so that a ".yaml" file gets a message saying that,
    /// instead of a JSON parse error.
    Yaml,
    Json,
    Json5,
    /// Recognised, but not parsed.  unitctl no longer reads hjson.  The format
    /// stays in this list so that an ".hjson" file gets a message saying that,
    /// instead of a JSON parse error.
    Hjson,
    Pem,
    JavaScript,
    Unknown,
}

impl InputFormat {
    pub fn from_file_extension<S>(file_extension: S) -> Self
    where
        S: Into<String>,
    {
        match file_extension.into().to_lowercase().as_str() {
            "yaml" => InputFormat::Yaml,
            "yml" => InputFormat::Yaml,
            "json" => InputFormat::Json,
            "json5" => InputFormat::Json5,
            "hjson" => InputFormat::Hjson,
            "cjson" => InputFormat::Hjson,
            "pem" => InputFormat::Pem,
            "js" => InputFormat::JavaScript,
            "njs" => InputFormat::JavaScript,
            _ => InputFormat::Unknown,
        }
    }

    /// This function allows us to infer the input format based on the remote path which is
    /// useful when processing input from STDIN.
    pub fn from_remote_path<S>(remote_path: S) -> Self
    where
        S: Into<String>,
    {
        let remote_upload_path = remote_path.into();
        let lead_slash_removed = remote_upload_path.trim_start_matches('/');
        let first_path = lead_slash_removed
            .split_once('/')
            .map_or(lead_slash_removed, |(first, _)| first);
        match first_path {
            "config" => InputFormat::Json,
            "certificates" => InputFormat::Pem,
            "js_modules" => InputFormat::JavaScript,
            _ => InputFormat::Json,
        }
    }
}

/// A "file" that can be used as input to a command
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InputFile {
    // Data received via STDIN
    Stdin(InputFormat),
    // Data that is on the file system where the format is inferred from the extension
    File(Box<Path>),
    // Data that is on the file system where the format is explicitly specified
    FileWithFormat(Box<Path>, InputFormat),
}

impl InputFile {
    /// Creates a new instance of `InputFile` from a string
    pub fn new<S>(file_path_or_dash: S, remote_path: S) -> Self
    where
        S: Into<String>,
    {
        let file_path: String = file_path_or_dash.into();

        match file_path.as_str() {
            "-" => InputFile::Stdin(InputFormat::from_remote_path(remote_path)),
            _ => InputFile::File(PathBuf::from(&file_path).into_boxed_path()),
        }
    }

    /// Returns the format of the input file
    pub fn format(&self) -> InputFormat {
        match self {
            InputFile::Stdin(format) => *format,
            InputFile::File(path) => {
                // Figure out the file format based on the file extension
                match path.extension().and_then(|s| s.to_str()) {
                    Some(ext) => InputFormat::from_file_extension(ext),
                    None => InputFormat::Unknown,
                }
            }
            InputFile::FileWithFormat(_file, format) => *format,
        }
    }

    pub fn mime_type(&self) -> String {
        match self.format() {
            InputFormat::Yaml => "application/x-yaml".to_string(),
            InputFormat::Json => "application/json".to_string(),
            InputFormat::Json5 => "application/json5".to_string(),
            InputFormat::Hjson => "application/hjson".to_string(),
            InputFormat::Pem => "application/x-pem-file".to_string(),
            InputFormat::JavaScript => "application/javascript".to_string(),
            InputFormat::Unknown => "application/octet-stream".to_string(),
        }
    }

    /// Returns true if the input file is in the format of a configuration file
    pub fn is_config(&self) -> bool {
        matches!(
            self.format(),
            InputFormat::Yaml | InputFormat::Json | InputFormat::Json5 | InputFormat::Hjson
        )
    }

    pub fn is_javascript(&self) -> bool {
        matches!(self.format(), InputFormat::JavaScript)
    }

    pub fn is_pem_bundle(&self) -> bool {
        matches!(self.format(), InputFormat::Pem)
    }

    /// Returns the path to the input file if it is a file and not a stream
    pub fn to_path(&self) -> Result<&Path, UnitctlError> {
        match self {
            InputFile::Stdin(_) => {
                let io_error = IoError::new(std::io::ErrorKind::InvalidInput, "Input file is stdin");
                Err(UnitctlError::IoError { source: io_error })
            }
            InputFile::File(path) | InputFile::FileWithFormat(path, _) => Ok(path),
        }
    }

    /// The media type and the bytes to PUT for a configuration input.
    ///
    /// The type is the type of the body, not of the file: a JSON5 file is
    /// parsed and sent as JSON, so it goes out as "application/json".
    ///
    /// JSON is sent as it was written.  unitctl does not parse it, so an
    /// operator's duplicate member reaches the server, which refuses it
    /// (src/nxt_conf.c:1616), and a number keeps the spelling the file used.
    /// Parsing here collapsed both silently.
    ///
    /// JSON5 is not JSON, so it is still parsed and re-serialized.  That is
    /// the one input whose bytes cannot go out unchanged.
    pub fn to_config_body(&self) -> Result<(String, KnownSize), UnitctlError> {
        let json = "application/json".to_string();

        match self.format() {
            InputFormat::Json => Ok((json, self.try_into()?)),
            InputFormat::Json5 => {
                let mut reader: Box<dyn BufRead + Send> = self.try_into()?;
                let mut json5_string = String::new();
                reader
                    .read_to_string(&mut json5_string)
                    .map_err(|e| UnitctlError::DeserializationError { message: e.to_string() })?;
                let parsed: UnitSerializableMap = json5::from_str(&json5_string)
                    .map_err(|e| UnitctlError::DeserializationError { message: e.to_string() })?;
                let body = serde_json::to_string(&parsed)
                    .map_err(|e| UnitctlError::SerializationError { message: e.to_string() })?;
                Ok((json, KnownSize::String(body)))
            }
            // Refuse hjson and YAML by name.  Sending the file as it is would
            // make the server report a syntax error on the first comment or
            // unquoted key, and that error does not tell the user what to do.
            InputFormat::Hjson => Err(UnitctlError::DeserializationError {
                message: "hjson is no longer supported: convert the file to JSON first".to_string(),
            }),
            InputFormat::Yaml => Err(UnitctlError::DeserializationError {
                message: "YAML is no longer supported: convert the file to JSON first, for example with \
                          \"yq -o=json\""
                    .to_string(),
            }),
            _ => Err(UnitctlError::DeserializationError {
                message: format!("Unsupported input format for a configuration: {:?}", self),
            }),
        }
    }
}

impl From<&Path> for InputFile {
    fn from(path: &Path) -> Self {
        InputFile::File(path.into())
    }
}

impl TryInto<Box<dyn BufRead + Send>> for &InputFile {
    type Error = UnitctlError;

    fn try_into(self) -> Result<Box<dyn BufRead + Send>, Self::Error> {
        let reader: Box<dyn BufRead + Send> = match self {
            InputFile::Stdin(_) => Box::new(BufReader::new(io::stdin())),
            InputFile::File(_) | InputFile::FileWithFormat(_, _) => {
                let path = self.to_path()?;
                let file = std::fs::File::open(path).map_err(|e| UnitctlError::IoError { source: e })?;
                let reader = Box::new(BufReader::new(file));
                Box::new(reader)
            }
        };
        Ok(reader)
    }
}

impl TryInto<Vec<u8>> for &InputFile {
    type Error = UnitctlError;

    fn try_into(self) -> Result<Vec<u8>, Self::Error> {
        let mut buf: Vec<u8> = vec![];
        let mut reader: Box<dyn BufRead + Send> = self.try_into()?;
        reader
            .read_to_end(&mut buf)
            .map_err(|e| UnitctlError::IoError { source: e })?;
        Ok(buf)
    }
}

impl TryInto<KnownSize> for &InputFile {
    type Error = UnitctlError;

    fn try_into(self) -> Result<KnownSize, Self::Error> {
        let known_size: KnownSize = match self {
            InputFile::Stdin(_) => {
                let mut buf: Vec<u8> = vec![];
                let _ = io::stdin()
                    .read_to_end(&mut buf)
                    .map_err(|e| UnitctlError::IoError { source: e })?;
                KnownSize::Vec(buf)
            }
            InputFile::File(_) | InputFile::FileWithFormat(_, _) => {
                let path = self.to_path()?;
                let file = std::fs::File::open(path).map_err(|e| UnitctlError::IoError { source: e })?;
                let len = file.metadata().map_err(|e| UnitctlError::IoError { source: e })?.len();
                let reader = Box::new(file);
                KnownSize::Read(reader, len)
            }
        };
        Ok(known_size)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn can_parse_file_extensions() {
        assert_eq!(InputFormat::from_file_extension("yaml"), InputFormat::Yaml);
        assert_eq!(InputFormat::from_file_extension("yml"), InputFormat::Yaml);
        assert_eq!(InputFormat::from_file_extension("json"), InputFormat::Json);
        assert_eq!(InputFormat::from_file_extension("json5"), InputFormat::Json5);
        assert_eq!(InputFormat::from_file_extension("hjson"), InputFormat::Hjson);
        assert_eq!(InputFormat::from_file_extension("cjson"), InputFormat::Hjson);
        assert_eq!(InputFormat::from_file_extension("pem"), InputFormat::Pem);
        assert_eq!(InputFormat::from_file_extension("js"), InputFormat::JavaScript);
        assert_eq!(InputFormat::from_file_extension("njs"), InputFormat::JavaScript);
        assert_eq!(InputFormat::from_file_extension("txt"), InputFormat::Unknown);
    }

    #[test]
    fn can_parse_remote_paths() {
        assert_eq!(InputFormat::from_remote_path("//config"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("/config"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("/config/"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("config/"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("config"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("/config/something/"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("config/something/"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("config/something"), InputFormat::Json);
        assert_eq!(InputFormat::from_remote_path("/certificates"), InputFormat::Pem);
        assert_eq!(InputFormat::from_remote_path("/certificates/"), InputFormat::Pem);
        assert_eq!(InputFormat::from_remote_path("certificates/"), InputFormat::Pem);
        assert_eq!(InputFormat::from_remote_path("certificates"), InputFormat::Pem);
        assert_eq!(InputFormat::from_remote_path("js_modules"), InputFormat::JavaScript);
        assert_eq!(InputFormat::from_remote_path("js_modules/"), InputFormat::JavaScript);

        assert_eq!(
            InputFormat::from_remote_path("/certificates/something/"),
            InputFormat::Pem
        );
        assert_eq!(
            InputFormat::from_remote_path("certificates/something/"),
            InputFormat::Pem
        );
        assert_eq!(
            InputFormat::from_remote_path("certificates/something"),
            InputFormat::Pem
        );
    }

    /// Member names in an order no map sorts into by accident, so a map that
    /// does not keep insertion order cannot pass by luck.
    const MEMBERS: [&str; 6] = [
        "settings",
        "listeners",
        "routes",
        "applications",
        "access_log",
        "upstreams",
    ];

    fn write_input(body: &str, suffix: &str) -> tempfile::NamedTempFile {
        let mut file = tempfile::Builder::new()
            .prefix("unitctl-inputfile-")
            .suffix(suffix)
            .tempfile()
            .expect("a temporary file");
        std::io::Write::write_all(file.as_file_mut(), body.as_bytes()).expect("writing the input");
        file
    }

    /// Read a body back without going through `into_full_body`, so the check
    /// does not depend on the same code it is checking.
    fn body_bytes(known_size: KnownSize) -> Vec<u8> {
        match known_size {
            KnownSize::Vec(bytes) => bytes,
            KnownSize::String(text) => text.into_bytes(),
            KnownSize::Read(mut reader, _) => {
                let mut bytes = Vec::new();
                reader.read_to_end(&mut bytes).expect("reading the body");
                bytes
            }
            KnownSize::Empty => Vec::new(),
        }
    }

    fn config_body(file: &tempfile::NamedTempFile, format: InputFormat) -> Vec<u8> {
        let (mime_type, body) = InputFile::FileWithFormat(file.path().into(), format)
            .to_config_body()
            .expect("the input must produce a body");

        // Every configuration body is JSON by the time it leaves, whatever the
        // file it came from.
        assert_eq!(mime_type, "application/json");

        body_bytes(body)
    }

    fn members_of(file: &tempfile::NamedTempFile, format: InputFormat) -> Vec<String> {
        let body = config_body(file, format);
        let map: UnitSerializableMap = serde_json::from_slice(&body).expect("the body must be JSON");
        map.keys().cloned().collect()
    }

    /// The body Unit receives keeps member order.  JSON is sent as it was
    /// written, so it can only keep it.  JSON5 is parsed and re-serialized, and
    /// both ends of that have to keep it.
    #[test]
    fn every_input_format_keeps_member_order() {
        let json = MEMBERS
            .iter()
            .map(|name| format!("  \"{}\": {{}}", name))
            .collect::<Vec<_>>()
            .join(",\n");
        let json = format!("{{\n{}\n}}\n", json);

        assert_eq!(members_of(&write_input(&json, ".json"), InputFormat::Json), MEMBERS);
        assert_eq!(members_of(&write_input(&json, ".json5"), InputFormat::Json5), MEMBERS);
    }

    /// A JSON file is sent byte for byte.  Parsing it here kept the last of two
    /// members with the same name and re-spelled every number, so the server
    /// never saw what the operator wrote.  The server refuses the duplicate
    /// itself (src/nxt_conf.c:1616).
    #[test]
    fn a_json_input_is_sent_unchanged() {
        let written = "{\n  \"listeners\": {},\n  \"listeners\": {\"*:8080\": {}},\n  \"settings\": 1.50\n}\n";
        let file = write_input(written, ".json");

        let body = config_body(&file, InputFormat::Json);

        assert_eq!(String::from_utf8(body).expect("the body is UTF-8"), written);
    }

    /// Bytes that are not valid UTF-8 now reach the server, which refuses them
    /// and names the member (src/nxt_conf_validation.c:1840).  unitctl refused
    /// them first, with a parse error that said nothing about the encoding.
    #[test]
    fn a_file_that_is_not_valid_utf8_is_sent_to_the_server() {
        let mut file = tempfile::Builder::new()
            .suffix(".json")
            .tempfile()
            .expect("a temporary file");
        let mut raw = br#"{"routes":[{"match":{"uri":"/"#.to_vec();
        raw.push(0xff);
        raw.extend_from_slice(br#""},"action":{"return":204}}]}"#);
        std::io::Write::write_all(file.as_file_mut(), &raw).expect("writing the input");

        let (_, body) = InputFile::FileWithFormat(file.path().into(), InputFormat::Json)
            .to_config_body()
            .expect("the bytes must be sent, not judged here");
        let body = body_bytes(body);

        assert_eq!(body, raw);
    }

    /// hjson is refused by name.  Handing the file to the JSON parser instead
    /// would report a syntax error on the first comment, which does not tell
    /// the user what to do.
    #[test]
    fn an_hjson_input_is_refused_with_a_message_about_hjson() {
        let file = write_input("{\n  # a comment\n  routes: []\n}\n", ".hjson");
        let Err(error) = InputFile::from(file.path()).to_config_body() else {
            panic!("hjson must be refused");
        };

        assert!(
            error.to_string().contains("hjson is no longer supported"),
            "unexpected message: {}",
            error
        );
    }

    /// YAML is refused by name for the same reason, and the ".yaml" extension
    /// still has to reach that refusal.  Were the variant dropped instead, the
    /// file would become `InputFormat::Unknown` and `execute` would abort on
    /// its `panic!("Unknown input file type")`.
    #[test]
    fn a_yaml_input_is_refused_with_a_message_about_yaml() {
        for suffix in [".yaml", ".yml"] {
            let file = write_input("routes: []\n", suffix);
            let input = InputFile::from(file.path());

            assert!(input.is_config(), "{} must stay a config format", suffix);

            let Err(error) = input.to_config_body() else {
                panic!("{} must be refused", suffix);
            };
            assert!(
                error.to_string().contains("YAML is no longer supported"),
                "unexpected message: {}",
                error
            );
        }
    }

    #[test]
    fn an_unsupported_input_format_is_refused() {
        let file = write_input("not a configuration", ".txt");
        assert!(InputFile::FileWithFormat(file.path().into(), InputFormat::Pem)
            .to_config_body()
            .is_err());
    }
}
