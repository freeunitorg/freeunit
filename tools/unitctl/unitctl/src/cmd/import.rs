use crate::inputfile::{InputFile, InputFormat};
use crate::unitctl::UnitCtl;
use crate::unitctl_error::UnitctlError;
use crate::{requests, wait};
use std::path::{Path, PathBuf};
use unit_client_rs::unit_client::{UnitClient, UnitSerializableMap};
use walkdir::{DirEntry, WalkDir};

enum UploadFormat {
    Config,
    PemBundle,
    Javascript,
}

impl TryFrom<&InputFile> for UploadFormat {
    type Error = UnitctlError;

    /// A file this command cannot place is refused, not fatal.  This used to
    /// panic on a format that reached it without a branch here.
    fn try_from(input_file: &InputFile) -> Result<Self, Self::Error> {
        if input_file.is_config() {
            Ok(UploadFormat::Config)
        } else if input_file.is_pem_bundle() {
            Ok(UploadFormat::PemBundle)
        } else if input_file.is_javascript() {
            Ok(UploadFormat::Javascript)
        } else {
            Err(UnitctlError::UnknownInputFileType {
                path: input_file.to_path()?.to_string_lossy().into(),
            })
        }
    }
}

impl UploadFormat {
    fn can_be_overwritten(&self) -> bool {
        matches!(self, UploadFormat::Config)
    }
    fn upload_path(&self, path: &Path) -> String {
        match self {
            UploadFormat::Config => "/config".to_string(),
            UploadFormat::PemBundle => format!("/certificates/{}.pem", Self::file_stem(path)),
            UploadFormat::Javascript => format!("/js_modules/{}.js", Self::file_stem(path)),
        }
    }

    fn file_stem(path: &Path) -> String {
        path.file_stem().unwrap_or_default().to_string_lossy().into()
    }
}

pub async fn cmd(cli: &UnitCtl, directory: &PathBuf) -> Result<(), UnitctlError> {
    if !directory.exists() {
        return Err(UnitctlError::PathNotFound {
            path: directory.to_string_lossy().into(),
        });
    }

    let clients: Vec<_> = wait::wait_for_sockets(cli)
        .await?
        .into_iter()
        .map(|sock| UnitClient::new(sock))
        .collect();

    let mut results = vec![];
    for i in WalkDir::new(directory)
        .follow_links(true)
        .sort_by_file_name()
        .into_iter()
        .filter_map(Result::ok)
        .filter(|e| !e.path().is_dir())
    {
        for client in &clients {
            results.push(process_entry(i.clone(), client).await);
        }
    }

    if results.iter().filter(|r| r.is_err()).count() == results.len() {
        Err(UnitctlError::NoFilesImported)
    } else {
        println!("Imported {} files", results.len());
        Ok(())
    }
}

async fn process_entry(entry: DirEntry, client: &UnitClient) -> Result<(), UnitctlError> {
    let input_file = InputFile::from(entry.path());
    if input_file.format() == InputFormat::Unknown {
        println!(
            "Skipping unknown file type: {}",
            input_file.to_path()?.to_string_lossy()
        );
        return Err(UnitctlError::UnknownInputFileType {
            path: input_file.to_path()?.to_string_lossy().into(),
        });
    }
    let upload_format = UploadFormat::try_from(&input_file)?;
    let upload_path = upload_format.upload_path(entry.path());

    // We can't overwrite JS or PEM files, so we delete them first
    if !upload_format.can_be_overwritten() {
        let _ = requests::send_empty_body_deserialize_response(client, "DELETE", upload_path.as_str())
            .await
            .ok();
    }

    let result = match upload_format {
        UploadFormat::Config => {
            requests::send_config_deserialize_response(client, "PUT", upload_path.as_str(), &input_file).await
        }
        UploadFormat::PemBundle => {
            requests::send_and_validate_pem_data_deserialize_response(client, "PUT", upload_path.as_str(), &input_file)
                .await
        }
        UploadFormat::Javascript => {
            requests::send_body_deserialize_response::<UnitSerializableMap>(
                client,
                "PUT",
                upload_path.as_str(),
                Some(&input_file),
            )
            .await
        }
    };

    match result {
        Ok(_) => {
            eprintln!(
                "Imported {} -> {}",
                input_file.to_path()?.to_string_lossy(),
                upload_path
            );
            Ok(())
        }
        Err(error) => {
            eprintln!("Error    {} -> {}", input_file.to_path()?.to_string_lossy(), error);
            Err(error)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(name: &str) -> InputFile {
        InputFile::from(Path::new(name))
    }

    /// Each format this command can place has its own destination, and the
    /// ".js" case is the one `execute` used to abort on.
    #[test]
    fn every_known_format_has_an_upload_path() {
        let cases = [
            ("conf.json", "/config"),
            ("bundle.pem", "/certificates/bundle.pem"),
            ("mod.js", "/js_modules/mod.js"),
        ];

        for (name, expected) in cases {
            let file = input(name);
            let format = UploadFormat::try_from(&file).expect("a known format");

            assert_eq!(format.upload_path(Path::new(name)), expected);
        }
    }

    /// A file this command cannot place is refused by name.  It used to
    /// panic, which gave the operator a backtrace instead of the file name.
    #[test]
    fn an_unplaceable_file_is_refused_not_fatal() {
        let Err(error) = UploadFormat::try_from(&input("notes.txt")) else {
            panic!("an unplaceable file must be refused");
        };

        assert!(
            matches!(error, UnitctlError::UnknownInputFileType { .. }),
            "unexpected error: {}",
            error
        );
    }
}
