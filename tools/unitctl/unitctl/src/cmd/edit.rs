use crate::inputfile::{InputFile, InputFormat};
use crate::requests::{send_and_validate_config_deserialize_response, send_empty_body_read_bytes};
use crate::unitctl::UnitCtl;
use crate::unitctl_error::ControlSocketErrorKind;
use crate::{wait, OutputFormat, UnitctlError};
use std::path::{Path, PathBuf};
use unit_client_rs::json_body;
use unit_client_rs::unit_client::{UnitClient, UnitSerializableMap};
use which::which;

const EDITOR_ENV_VARS: [&str; 2] = ["EDITOR", "VISUAL"];
const EDITOR_KNOWN_LIST: [&str; 8] = [
    "sensible-editor",
    "editor",
    "vim",
    "nano",
    "nvim",
    "vi",
    "pico",
    "emacs",
];

pub(crate) async fn cmd(cli: &UnitCtl, output_format: OutputFormat) -> Result<(), UnitctlError> {
    if cli.control_socket_addresses.as_ref().map_or(false, |addrs| addrs.len() > 1) {
        return Err(UnitctlError::ControlSocketError {
            kind: ControlSocketErrorKind::General,
            message: "too many control sockets. specify at most one.".to_string(),
        });
    }

    let mut control_sockets = wait::wait_for_sockets(cli).await?;
    let client = UnitClient::new(control_sockets.pop().unwrap());
    // Get latest configuration.  Editing means handing it back, and the way
    // back runs the file through serde_json, whose strings are Rust strings:
    // bytes Unit stores but UTF-8 cannot carry would be replaced by the mere
    // act of opening the editor.  Refuse rather than rewrite a configuration
    // the user only meant to look at.
    // UnitSerializableMap and not serde_json::Value: a configuration is a JSON
    // object, and the type is what keeps a 200 carrying "null", an array or a
    // bare string -- an intermediary talking -- from being opened in the editor
    // and handed back as the whole configuration.
    let raw_config = send_empty_body_read_bytes(&client, "GET", "/config").await?;
    let current_config: UnitSerializableMap = match json_body::decode(&raw_config) {
        Ok((config, fidelity)) if fidelity.is_exact() => config,
        Ok((_, fidelity)) => {
            return Err(UnitctlError::UndecodableConfiguration {
                path: "/config".to_string(),
                members: fidelity.members(),
            })
        }
        // Name the path, as the JsonError this replaced did: "expected value at
        // line 1 column 1" on its own does not say what was being read -- and
        // show the body, because a 200 carrying an intermediary's page is the
        // ordinary way this arm is reached.
        Err(error) => {
            return Err(UnitctlError::DeserializationError {
                message: format!("/config: {}\n{}", error, json_body::body_for_display(&raw_config)),
            })
        }
    };

    // Write JSON to temporary file - this file will automatically be deleted by the OS when
    // the last file handle to it is removed.
    let mut temp_file = tempfile::Builder::new()
        .prefix("unitctl-")
        .suffix(".json")
        .tempfile()
        .map_err(|e| UnitctlError::IoError { source: e })?;

    // Pretty format JSON received from Unit and write to the temporary file
    serde_json::to_writer_pretty(temp_file.as_file_mut(), &current_config)
        .map_err(|e| UnitctlError::SerializationError { message: e.to_string() })?;

    // Load edited file
    let temp_file_path = temp_file.path();
    let before_edit_mod_time = temp_file_path.metadata().ok().map(|m| m.modified().ok());

    let inputfile = InputFile::FileWithFormat(temp_file_path.into(), InputFormat::Json5);
    open_editor(temp_file_path)?;
    let after_edit_mod_time = temp_file_path.metadata().ok().map(|m| m.modified().ok());

    // Check if file was modified before sending to Unit
    if let (Some(before), Some(after)) = (before_edit_mod_time, after_edit_mod_time) {
        if before == after {
            eprintln!("File was not modified - no changes will be sent to Unit");
            return Ok(());
        }
    };

    // Send edited file to Unit to overwrite current configuration
    send_and_validate_config_deserialize_response(&client, "PUT", "/config", Some(&inputfile))
        .await
        .and_then(|status| output_format.write_to_stdout(&status))
}

/// Look for an editor in the environment variables
fn find_editor_from_env() -> Option<PathBuf> {
    EDITOR_ENV_VARS
        .iter()
        .filter_map(std::env::var_os)
        .filter(|s| !s.is_empty())
        .filter_map(|s| which(s).ok())
        .filter_map(|path| path.canonicalize().ok())
        .find(|path| path.exists())
}

/// Look for editor in path by matching against a list of known editors or aliases
fn find_editor_from_known_list() -> Option<PathBuf> {
    EDITOR_KNOWN_LIST
        .iter()
        .filter_map(|editor| which(editor).ok())
        .filter_map(|path| path.canonicalize().ok())
        .find(|editor| editor.exists())
}

/// Find the path to an editor
pub fn find_editor_path() -> Result<PathBuf, UnitctlError> {
    find_editor_from_env()
        .or_else(find_editor_from_known_list)
        .ok_or_else(|| UnitctlError::EditorError {
            message: "Could not find an editor".to_string(),
        })
}

/// Start an editor with a given path
pub fn open_editor(path: &Path) -> Result<(), UnitctlError> {
    let editor_path = find_editor_path()?;
    let status = std::process::Command::new(editor_path)
        .arg(path)
        .status()
        .map_err(|e| UnitctlError::EditorError {
            message: format!("Could not open editor: {}", e),
        })?;
    if status.success() {
        Ok(())
    } else {
        Err(UnitctlError::EditorError {
            message: format!("Editor exited with non-zero status: {}", status),
        })
    }
}
