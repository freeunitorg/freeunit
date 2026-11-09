use crate::unitd_cmd::UnitdCmd;
#[cfg(target_os = "linux")]
use crate::unitd_docker::pid_is_dockerized;
use crate::unitd_docker::UnitdContainer;
use crate::unitd_instance::UNITD_BINARY_NAMES;
use crate::unitd_process_user::UnitdProcessUser;
use serde::ser::SerializeMap;
use serde::{Serialize, Serializer};
use std::collections::HashMap;
use std::path::Path;
use sysinfo::{Pid, Process, ProcessRefreshKind, System, UpdateKind, Users};

#[derive(Debug, Clone)]
pub struct UnitdProcess {
    pub binary_name: String,
    pub process_id: u64,
    pub executable_path: Option<Box<Path>>,
    pub environ: Vec<String>,
    pub all_cmds: Vec<String>,
    pub working_dir: Option<Box<Path>>,
    pub child_pids: Vec<u64>,
    pub user: Option<UnitdProcessUser>,
    pub effective_user: Option<UnitdProcessUser>,
    pub container: Option<UnitdContainer>,
}

impl Serialize for UnitdProcess {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        // 6 = fields to serialize
        let mut state = serializer.serialize_map(Some(6))?;
        state.serialize_entry("pid", &self.process_id)?;
        state.serialize_entry("user", &self.user)?;
        state.serialize_entry("effective_user", &self.effective_user)?;
        state.serialize_entry("executable", &self.executable_path())?;
        state.serialize_entry("child_pids", &self.child_pids)?;
        state.serialize_entry("container", &self.container)?;
        state.end()
    }
}

// Trust only a unitd owned by root (uid 0) or by the user running unitctl; an
// owner uid that cannot be read fails closed.  The real unitd main process
// never drops privileges, so an unprivileged decoy is rejected.
fn trusted_owner(owner: Option<u32>, current_uid: Option<u32>) -> bool {
    match owner {
        Some(uid) => uid == 0 || Some(uid) == current_uid,
        None => false,
    }
}

impl UnitdProcess {
    pub fn find_unitd_processes() -> Vec<UnitdProcess> {
        let process_refresh_kind = ProcessRefreshKind::nothing()
            .with_cmd(UpdateKind::Always)
            .with_cwd(UpdateKind::Always)
            .with_exe(UpdateKind::Always)
            .with_user(UpdateKind::Always);
        let refresh_kind = sysinfo::RefreshKind::nothing().with_processes(process_refresh_kind);
        // sysinfo 0.40 returns an error when the process list cannot be read.
        let sys = match System::new_with_specifics(refresh_kind) {
            Ok(sys) => sys,
            Err(error) => {
                eprintln!("Failed to list processes: {}", error);
                return Vec::new();
            }
        };

        // An unprivileged local user can plant a decoy process named "unitd"
        // whose command line reads "unit: main ...".  The real unitd main
        // process never drops privileges, so it runs as root (or, for a
        // rootless instance, as the user running unitctl).  Resolve that user's
        // uid from our own process so a decoy owned by anyone else is ignored;
        // this mirrors the unitc/setup-unit shell tools.
        let current_uid: Option<u32> = sysinfo::get_current_pid()
            .ok()
            .and_then(|pid| sys.process(pid))
            .and_then(|process| process.user_id())
            .map(|uid| *uid.clone());

        let unitd_processes: HashMap<&Pid, &Process> = sys
            .processes()
            .iter()
            .filter(|p| {
                let process_name = p.1.name();
                UNITD_BINARY_NAMES.iter().any(|&name| process_name == name)
            })
            // Trust only a unitd owned by root or by the current user.
            .filter(|p| trusted_owner(p.1.user_id().map(|uid| *uid.clone()), current_uid))
            .collect::<HashMap<&Pid, &Process>>();
        // Without the user list, the user names are left empty.
        let users = Users::new_with_refreshed_list()
            .map_err(|error| eprintln!("Failed to list users: {}", error))
            .ok();

        unitd_processes
            .iter()
            // Filter out child processes
            .filter(|p| {
                #[cfg(target_os = "linux")]
                if pid_is_dockerized(p.0.as_u32().into()) {
                    return false;
                }
                let parent_pid = p.1.parent();
                match parent_pid {
                    Some(pid) => !unitd_processes.contains_key(&pid),
                    None => false,
                }
            })
            .map(|p| {
                let tuple = p.to_owned();
                /* The sysinfo library only supports 32-bit pids, yet larger values are possible
                 * if the OS is configured to support it, thus we use 64-bit integers internally
                 * because it is just a matter of time until the library changes to larger values. */
                let pid = *tuple.0;
                let process = *tuple.1;
                let process_id: u64 = pid.as_u32().into();
                let executable_path: Option<Box<Path>> = process.exe().map(|p| p.to_path_buf().into_boxed_path());
                let environ: Vec<String> = process
                    .environ()
                    .iter()
                    .map(|s| s.to_string_lossy().into_owned())
                    .collect();
                let cmd: Vec<String> = process.cmd().iter().map(|s| s.to_string_lossy().into_owned()).collect();
                let working_dir: Option<Box<Path>> = process.cwd().map(|p| p.to_path_buf().into_boxed_path());
                let child_pids = unitd_processes
                    .iter()
                    .filter_map(|p| p.to_owned().1.parent())
                    .filter(|parent_pid| parent_pid == pid)
                    .map(|p| p.as_u32() as u64)
                    .collect::<Vec<u64>>();

                let user = process
                    .user_id()
                    .and_then(|uid| users.as_ref()?.get_user_by_id(uid))
                    .map(UnitdProcessUser::from);
                let effective_user = process
                    .effective_user_id()
                    .and_then(|uid| users.as_ref()?.get_user_by_id(uid))
                    .map(UnitdProcessUser::from);

                UnitdProcess {
                    binary_name: process.name().to_string_lossy().into_owned(),
                    process_id,
                    executable_path,
                    environ,
                    all_cmds: cmd,
                    working_dir,
                    child_pids,
                    user,
                    effective_user,
                    container: None,
                }
            })
            .collect::<Vec<UnitdProcess>>()
    }

    pub fn cmd(&self) -> Option<UnitdCmd> {
        if self.all_cmds.is_empty() {
            return None;
        }

        match UnitdCmd::new(self.all_cmds[0].clone(), self.binary_name.as_ref()) {
            Ok(cmd) => Some(cmd),
            Err(error) => {
                eprintln!("Failed to parse process cmd: {}", error);
                None
            }
        }
    }

    pub fn executable_path(&self) -> Option<Box<Path>> {
        if self.executable_path.is_some() {
            return self.executable_path.clone();
        }
        self.cmd().and_then(|cmd| cmd.process_executable_path)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn can_parse_runtime_cmd_absolute_path(binary_name: &str) {
        let cmd = format!(
            "unit: main v1.28.0 [/usr/sbin/{} --log /var/log/unit.log --pid /var/run/unit.pid]",
            binary_name
        );
        let unitd_cmd = UnitdCmd::new(cmd, binary_name).expect("Failed to parse unitd cmd");
        assert_eq!(unitd_cmd.version.unwrap(), "1.28.0");
        assert_eq!(
            unitd_cmd.process_executable_path.unwrap().to_string_lossy(),
            format!("/usr/sbin/{}", binary_name)
        );
        let flags = unitd_cmd.flags.unwrap();
        assert_eq!(flags.get_flag_value("log").unwrap(), "/var/log/unit.log");
        assert_eq!(flags.get_flag_value("pid").unwrap(), "/var/run/unit.pid");
    }

    fn can_parse_runtime_cmd_relative_path(binary_name: &str) {
        let cmd = format!(
            "unit: main v1.29.0 [./sbin/{} --no-daemon --tmp /tmp --something]",
            binary_name
        );
        let unitd_cmd = UnitdCmd::new(cmd, binary_name).expect("Failed to parse unitd cmd");
        assert_eq!(unitd_cmd.version.unwrap(), "1.29.0");
        assert_eq!(
            unitd_cmd.process_executable_path.unwrap().to_string_lossy(),
            format!("./sbin/{}", binary_name)
        );
        let flags = unitd_cmd.flags.unwrap();
        assert_eq!(flags.get_flag_value("tmp").unwrap(), "/tmp");
        assert!(flags.has_flag("something"));
    }

    #[test]
    fn can_parse_runtime_cmd_unitd_absolute_path() {
        can_parse_runtime_cmd_absolute_path("unitd");
    }

    #[test]
    fn can_parse_runtime_cmd_unitd_debug_absolute_path() {
        can_parse_runtime_cmd_absolute_path("unitd-debug");
    }

    #[test]
    fn can_parse_runtime_cmd_unitd_relative_path() {
        can_parse_runtime_cmd_relative_path("unitd");
    }

    #[test]
    fn can_parse_runtime_cmd_unitd_debug_relative_path() {
        can_parse_runtime_cmd_relative_path("unitd-debug");
    }

    #[test]
    fn trusted_owner_accepts_root() {
        // root-owned unitd is the normal case, trusted even if our own uid is
        // unknown.
        assert!(trusted_owner(Some(0), Some(1000)));
        assert!(trusted_owner(Some(0), None));
    }

    #[test]
    fn trusted_owner_accepts_self() {
        // a rootless instance owned by the user running unitctl.
        assert!(trusted_owner(Some(1000), Some(1000)));
    }

    #[test]
    fn trusted_owner_rejects_another_user() {
        // a decoy owned by a different user is never trusted.
        assert!(!trusted_owner(Some(1000), Some(1001)));
        assert!(!trusted_owner(Some(1000), None));
    }

    #[test]
    fn trusted_owner_rejects_unreadable_owner() {
        // fail closed when the owner uid cannot be read.
        assert!(!trusted_owner(None, Some(1000)));
        assert!(!trusted_owner(None, None));
    }
}
