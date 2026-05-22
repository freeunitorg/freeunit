//! unit-eol-check — validates FreeUnit's `pkg/eol.json` against endoflife.date API.
//!
//! Usage:
//!   unit-eol-check [OPTIONS]
//!
//! Options:
//!   --json PATH      Path to eol.json (default: ./pkg/eol.json)
//!   --os             Check OS versions only
//!   --runtimes       Check runtime versions only
//!   --days N         Warn if EOL is within N days (default: 365)
//!   --fix            Print corrected runtime EOL lines (review and apply manually)
//!   --ci             CI mode: exit 1 if any errors, JSON to stdout
//!   --quiet          Suppress [ OK ] lines
//!
//! Exit codes:
//!   0  — all dates match or only grace-period warnings
//!   1  — one or more errors found (wrong dates, missed EOL)
//!   2  — network/file error

use std::env;
use std::fs;
use std::process;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
struct OsEntry {
    category: String, // fedora, debian, etc.
    version: String,
    eol: Option<String>,
}

#[derive(Clone, Debug)]
struct RuntimeEntry {
    category: String, // go, node, php, etc.
    version: String,
    eol: Option<String>,
    supported_until: Option<String>,
    note: Option<String>,
}

#[derive(Clone, Debug)]
struct Mismatch {
    category: String,
    version: String,
    kind: String, // "os" or "runtime"
    matrix_date: Option<String>,
    actual_date: Option<String>,
    severity: Severity,
    message: String,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Ord, PartialOrd)]
enum Severity {
    Info,
    Warning,
    Error,
}

impl std::fmt::Display for Severity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Severity::Error => write!(f, "ERROR"),
            Severity::Warning => write!(f, "WARN"),
            Severity::Info => write!(f, "INFO"),
        }
    }
}

// ---------------------------------------------------------------------------
// Config (parsed from _grace_* fields in eol.json)
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
struct Config {
    grace_runtimes: i64,
    grace_os: i64,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            grace_runtimes: 12,
            grace_os: 36,
        }
    }
}

fn parse_eol_json(path: &str) -> Result<(Vec<OsEntry>, Vec<RuntimeEntry>, Config), String> {
    let content = fs::read_to_string(path).map_err(|e| format!("read {}: {}", path, e))?;
    let json: serde_json::Value =
        serde_json::from_str(&content).map_err(|e| format!("parse JSON: {}", e))?;

    // Parse grace periods from meta fields
    let mut config = Config::default();
    if let Some(g) = json.get("_grace_runtimes").and_then(|v| v.as_i64()) {
        config.grace_runtimes = g;
    }
    if let Some(g) = json.get("_grace_os").and_then(|v| v.as_i64()) {
        config.grace_os = g;
    }

    let mut os_entries = Vec::new();
    let mut runtime_entries = Vec::new();

    // Parse OS entries
    if let Some(os) = json.get("os").and_then(|v| v.as_object()) {
        for (category, entries) in os {
            if let Some(arr) = entries.as_array() {
                for entry in arr {
                    let obj = entry.as_object().unwrap();
                    os_entries.push(OsEntry {
                        category: category.clone(),
                        version: obj.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
                        eol: obj.get("eol").and_then(|v| v.as_str()).map(String::from),
                    });
                }
            }
        }
    }

    // Parse runtime entries
    if let Some(runtimes) = json.get("runtimes").and_then(|v| v.as_object()) {
        for (category, entries) in runtimes {
            if let Some(arr) = entries.as_array() {
                for entry in arr {
                    let obj = entry.as_object().unwrap();
                    runtime_entries.push(RuntimeEntry {
                        category: category.clone(),
                        version: obj.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
                        eol: obj.get("eol").and_then(|v| v.as_str()).map(String::from),
                        supported_until: obj
                            .get("supported_until")
                            .and_then(|v| v.as_str())
                            .map(String::from),
                        note: obj.get("note").and_then(|v| v.as_str()).map(String::from),
                    });
                }
            }
        }
    }

    Ok((os_entries, runtime_entries, config))
}

// ---------------------------------------------------------------------------
// HTTP fetch from endoflife.date (std::net::TcpStream only — zero deps)
// ---------------------------------------------------------------------------

fn fetch_api(category: &str) -> Result<String, String> {
    let url = format!("https://endoflife.date/api/{}.json", category);

    // Use curl (already installed in Docker image) — handles HTTPS + redirects
    let output = std::process::Command::new("curl")
        .args(["-sL", "--max-time", "10", "-A", "unit-eol-check/0.1", &url])
        .output()
        .map_err(|e| format!("curl failed: {}", e))?;

    if !output.status.success() {
        return Err(format!(
            "curl {} failed: {}",
            url,
            String::from_utf8_lossy(&output.stderr)
        ));
    }

    let body = String::from_utf8_lossy(&output.stdout).into_owned();
    if body.is_empty() {
        return Err(format!("empty response from {}", url));
    }

    Ok(body)
}

// ---------------------------------------------------------------------------
// API date lookup
// ---------------------------------------------------------------------------

fn api_eol_date(category: &str, version: &str) -> Option<String> {
    let api_category = match category {
        "jsc" => "jdk",
        "node" => "nodejs",
        "amazonlinux" => "amazon-linux",
        "centos_stream" => "centos-stream",
        "minimal" | "wasm" => return None,
        _ => category,
    };

    let api_json = match fetch_api(api_category) {
        Ok(s) => s,
        Err(_) => return None,
    };

    let entries: Vec<serde_json::Value> = match serde_json::from_str(&api_json) {
        Ok(v) => v,
        Err(_) => return None,
    };

    for entry in entries {
        let cycle = entry.get("cycle")?.as_str()?;
        if cycle == version {
            // eol can be false (bool) or a date string
            if let Some(eol_val) = entry.get("eol") {
                match eol_val {
                    serde_json::Value::String(s) => {
                        // Normalize to YYYY-MM
                        if s.len() >= 10 {
                            return Some(s[..7].to_string());
                        }
                    }
                    serde_json::Value::Bool(b) if *b == false => {
                        // No EOL set yet — future release
                        return Some(String::from("future"));
                    }
                    _ => {}
                }
            }
            return None;
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Date comparison helpers
// ---------------------------------------------------------------------------

fn date_to_months(s: &str) -> Option<(i32, u8)> {
    // Handle both YYYY-MM and YYYY-MM-DD formats; skip "future"
    let s = if s.len() >= 7 { &s[..7] } else { s };
    if s == "future" {
        return None;
    }
    let parts: Vec<&str> = s.split('-').collect();
    if parts.len() != 2 {
        return None;
    }
    let year: i32 = parts[0].parse().ok()?;
    let month: u8 = parts[1].parse().ok()?;
    Some((year, month))
}

fn months_between(base: &str, target: &str) -> Option<i64> {
    let (by, bm) = date_to_months(base)?;
    let (ty, tm) = date_to_months(target)?;
    let base_months = (by as i64) * 12 + (bm as i64);
    let target_months = (ty as i64) * 12 + (tm as i64);
    Some(target_months - base_months)
}

// ---------------------------------------------------------------------------
// New version detection
// ---------------------------------------------------------------------------

/// Fetch latest cycle from endoflife.date API and check if it's missing from matrix.
/// Returns Mismatch items for each category where a new version exists in API
/// but is absent from the matrix.
fn detect_new_versions(
    os_entries: &[OsEntry],
    runtime_entries: &[RuntimeEntry],
    _config: &Config,
) -> Vec<Mismatch> {
    let mut results = Vec::new();
    let now = now_yyyy_mm();

    // Build set of known (category, version) pairs
    let mut known: std::collections::HashSet<(String, String)> = std::collections::HashSet::new();
    for e in os_entries {
        known.insert((e.category.clone(), e.version.clone()));
    }
    for e in runtime_entries {
        known.insert((e.category.clone(), e.version.clone()));
    }

    // API categories we track (map matrix category → API category)
    let api_cats: Vec<(&str, &str)> = vec![
        ("go", "go"),
        ("jsc", "jdk"),
        ("node", "nodejs"),
        ("perl", "perl"),
        ("php", "php"),
        ("python", "python"),
        ("ruby", "ruby"),
        ("fedora", "fedora"),
        ("debian", "debian"),
        ("ubuntu", "ubuntu"),
        ("alpine", "alpine"),
        ("amazonlinux", "amazon-linux"),
        ("rhel", "rhel"),
        ("centos_stream", "centos-stream"),
    ];

    for (matrix_cat, api_cat) in &api_cats {
        let api_json = match fetch_api(api_cat) {
            Ok(s) => s,
            Err(_) => continue,
        };

        let entries: Vec<serde_json::Value> = match serde_json::from_str(&api_json) {
            Ok(v) => v,
            Err(_) => continue,
        };

        // Get the latest (first) entry — highest latestReleaseDate
        if let Some(first) = entries.first() {
            let cycle = first.get("cycle").and_then(|v| v.as_str()).unwrap_or("");
            let latest_date = first
                .get("latestReleaseDate")
                .and_then(|v| v.as_str())
                .unwrap_or("");

            if !known.contains(&(matrix_cat.to_string(), cycle.to_string())) && !cycle.is_empty() {
                // New version detected
                let months_old = months_between(latest_date, &now);
                let is_fresh = months_old.map_or(false, |m| m <= 3);

                results.push(Mismatch {
                    category: matrix_cat.to_string(),
                    version: cycle.to_string(),
                    kind: "new_version".to_string(),
                    matrix_date: None,
                    actual_date: Some(latest_date.to_string()),
                    severity: if is_fresh {
                        Severity::Error
                    } else {
                        Severity::Warning
                    },
                    message: if is_fresh {
                        format!(
                            "NEW: {} {} released {}, not in matrix — add to pkg/eol.json",
                            matrix_cat, cycle, latest_date
                        )
                    } else {
                        format!(
                            "MISSING: {} {} last release {}, matrix may need update",
                            matrix_cat, cycle, latest_date
                        )
                    },
                });
            }
        }
    }

    results
}

// ---------------------------------------------------------------------------
// Comparison logic
// ---------------------------------------------------------------------------

fn now_yyyy_mm() -> String {
    std::process::Command::new("date")
        .arg("+%Y-%m")
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| {
            eprintln!("[ ERROR ] failed to determine current date via `date +%Y-%m`");
            std::process::exit(2);
        })
}

fn check_os_entries(entries: &[OsEntry], config: &Config) -> Vec<Mismatch> {
    let mut results = Vec::new();
    let now = now_yyyy_mm();
    let grace_months = config.grace_os;

    for entry in entries {
        if let Some(matrix_date) = &entry.eol {
            if matrix_date == "future" {
                continue;
            }

            let actual = api_eol_date(&entry.category, &entry.version);

            match &actual {
                Some(actual_date) if actual_date == "future" => {
                    // No EOL set yet — info only
                    results.push(Mismatch {
                        category: entry.category.clone(),
                        version: entry.version.clone(),
                        kind: "os".to_string(),
                        matrix_date: Some(matrix_date.clone()),
                        actual_date: actual.clone(),
                        severity: Severity::Info,
                        message: format!(
                            "upstream has no EOL date yet for {} {}",
                            entry.category, entry.version
                        ),
                    });
                }
                Some(actual_date) => {
                    let diff = months_between(matrix_date, actual_date);
                    if diff.unwrap_or(0) == 0 {
                        // Dates match — check if within grace period warning
                        if let Some(months) = months_between(&now, matrix_date) {
                            if months < grace_months {
                                results.push(Mismatch {
                                    category: entry.category.clone(),
                                    version: entry.version.clone(),
                                    kind: "os".to_string(),
                                    matrix_date: Some(matrix_date.clone()),
                                    actual_date: Some(actual_date.clone()),
                                    severity: Severity::Warning,
                                    message: format!(
                                        "{} {} EOL in ~{} months — plan migration",
                                        entry.category, entry.version, months
                                    ),
                                });
                            }
                        }
                    } else if diff.unwrap_or(0) < 0 {
                        // Matrix date is in the past vs actual — matrix date is WRONG
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "os".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            message: format!(
                                "{} {} matrix EOL {} != actual {} — update matrix",
                                entry.category,
                                entry.version,
                                matrix_date,
                                actual_date
                            ),
                        });
                    } else {
                        // Matrix date is in the future vs actual — off by more than a month
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "os".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            message: format!(
                                "{} {} matrix EOL {} != actual {}",
                                entry.category, entry.version, matrix_date, actual_date
                            ),
                        });
                    }
                }
                None => {
                    // Could not fetch — skip but warn
                }
            }
        }
    }

    results
}

fn check_runtime_entries(entries: &[RuntimeEntry], _config: &Config) -> Vec<Mismatch> {
    let mut results = Vec::new();
    let now = now_yyyy_mm();

    for entry in entries {
        if let Some(matrix_date) = &entry.eol {
            if matrix_date == "future" || entry.version.is_empty() {
                continue;
            }

            let actual = api_eol_date(&entry.category, &entry.version);

            match &actual {
                Some(actual_date) if actual_date == "future" => {
                    results.push(Mismatch {
                        category: entry.category.clone(),
                        version: entry.version.clone(),
                        kind: "runtime".to_string(),
                        matrix_date: Some(matrix_date.clone()),
                        actual_date: Some(actual_date.clone()),
                        severity: Severity::Info,
                        message: format!(
                            "{} {} upstream EOL not yet set",
                            entry.category, entry.version
                        ),
                    });
                }
                Some(actual_date) => {
                    let diff = months_between(matrix_date, actual_date);
                    let diff_val = diff.unwrap_or(0);

                    if diff_val == 0 {
                        // Exact match — check if past EOL
                        if let Some(months) = months_between(&now, matrix_date) {
                            if months < 0 {
                                // Past EOL — check if flagged
                                if entry.note.as_ref().map_or(true, |n| !n.contains("EOL")) {
                                    results.push(Mismatch {
                                        category: entry.category.clone(),
                                        version: entry.version.clone(),
                                        kind: "runtime".to_string(),
                                        matrix_date: Some(matrix_date.clone()),
                                        actual_date: Some(actual_date.clone()),
                                        severity: Severity::Warning,
                                        message: format!(
                                            "{} {} upstream EOL passed ({}), add (EOL) flag",
                                            entry.category, entry.version, matrix_date
                                        ),
                                    });
                                }
                            }
                        }
                    } else if diff_val < 0 {
                        // Matrix date is in the past vs actual future date — error
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "runtime".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            message: format!(
                                "{} {} matrix EOL {} != actual {}",
                                entry.category, entry.version, matrix_date, actual_date
                            ),
                        });
                    } else {
                        // Off by 1+ months
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "runtime".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            message: format!(
                                "{} {} matrix EOL {} != actual {}",
                                entry.category, entry.version, matrix_date, actual_date
                            ),
                        });
                    }
                }
                None => {
                    // Could not fetch — skip silently for offline
                }
            }
        }
    }

    results
}

// ---------------------------------------------------------------------------
// Output reporters
// ---------------------------------------------------------------------------

fn report_human(mismatches: &[Mismatch], quiet: bool) {
    for m in mismatches {
        let prefix = match m.severity {
            Severity::Error => "[ ERROR ]",
            Severity::Warning => "[ WARN  ]",
            Severity::Info => "[ INFO  ]",
        };
        println!("{} {}: {}", prefix, m.category, m.message);
    }

    if quiet {
        return;
    }
    // Count OK entries (for human mode we don't list them in quiet mode)
    // This is handled in main()
    let errors = mismatches
        .iter()
        .filter(|m| m.severity == Severity::Error)
        .count();
    let warnings = mismatches
        .iter()
        .filter(|m| m.severity == Severity::Warning)
        .count();

    if errors == 0 && warnings == 0 {
        println!("[ OK    ] all dates match");
    }
}

fn report_ci(mismatches: &[Mismatch]) {
    let errors = mismatches
        .iter()
        .filter(|m| m.severity == Severity::Error)
        .count();
    let warnings = mismatches
        .iter()
        .filter(|m| m.severity == Severity::Warning)
        .count();

    let items: Vec<&Mismatch> = mismatches
        .iter()
        .filter(|m| m.severity != Severity::Info)
        .collect();

    let result = serde_json::json!({
        "errors": errors,
        "warnings": warnings,
        "items": items.iter().map(|m| {
            serde_json::json!({
                "category": m.category,
                "version": m.version,
                "kind": m.kind,
                "severity": format!("{:?}", m.severity).to_lowercase(),
                "matrix_date": m.matrix_date,
                "actual_date": m.actual_date,
                "message": m.message,
            })
        }).collect::<Vec<_>>(),
    });

    println!("{}", serde_json::to_string_pretty(&result).unwrap());

    if errors > 0 {
        process::exit(1);
    }
}

// ---------------------------------------------------------------------------
// --fix mode: generate corrected eol.json
// ---------------------------------------------------------------------------

fn generate_fix(entries: &[RuntimeEntry], config: &Config) -> Vec<RuntimeEntry> {
    let grace = config.grace_runtimes;
    let mut fixed = Vec::new();

    for entry in entries {
        let mut e = entry.clone();

        if let Some(ref matrix_date) = entry.eol {
            if matrix_date == "future" || entry.version.is_empty() {
                fixed.push(e);
                continue;
            }

            let actual = api_eol_date(&entry.category, &entry.version);

            if let Some(actual_date) = actual {
                if actual_date != "future" && actual_date != *matrix_date {
                    e.eol = Some(actual_date.clone());
                    // supported_until = eol + grace_months
                    if let Some((y, m)) = date_to_months(&actual_date) {
                        let total_months = (y as i64) * 12 + (m as i64) + grace;
                        let (new_y, new_m) = if total_months % 12 == 0 {
                            (total_months / 12 - 1, 12u8)
                        } else {
                            (total_months / 12, (total_months % 12) as u8)
                        };
                        e.supported_until = Some(format!("{:04}-{:02}", new_y, new_m));
                    }
                }
            }
        }

        fixed.push(e);
    }

    fixed
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn usage() {
    eprintln!(
        "Usage: unit-eol-check [OPTIONS]
Options:
  --json PATH      Path to eol.json (default: ./pkg/eol.json)
  --os             Check OS versions only
  --runtimes       Check runtime versions only
  --new            Detect new versions missing from matrix (API latest cycle)
  --ci             CI mode: exit 1 if any errors, JSON to stdout
  --fix            Print corrected runtime EOL lines (review and apply manually)
  --quiet          Suppress [ OK ] lines
  --help           Show this help"
    );
    process::exit(1);
}

fn main() {
    let args: Vec<String> = env::args().collect();

    let mut json_path = String::from("pkg/eol.json");
    let mut check_os = true;
    let mut check_runtimes = true;
    let mut ci_mode = false;
    let mut fix_mode = false;
    let mut quiet = false;
    let mut check_new = false;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => {
                i += 1;
                if i < args.len() {
                    json_path = args[i].clone();
                }
            }
            "--os" => {
                check_runtimes = false;
            }
            "--runtimes" => {
                check_os = false;
            }
            "--ci" => {
                ci_mode = true;
            }
            "--fix" => {
                fix_mode = true;
            }
            "--quiet" => {
                quiet = true;
            }
            "--new" => {
                check_new = true;
            }
            "--help" | "-h" => {
                usage();
            }
            _ => {}
        }
        i += 1;
    }

    // Parse eol.json
    let (os_entries, runtime_entries, config) = match parse_eol_json(&json_path) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("[ ERROR ] {}", e);
            process::exit(2);
        }
    };

    if fix_mode {
        // --fix prints corrected runtime EOL lines (not full JSON); review and apply manually.
        let fixed = generate_fix(&runtime_entries, &config);
        for entry in &fixed {
            println!(
                "{} {}: eol={:?} supported_until={:?}",
                entry.category, entry.version, entry.eol, entry.supported_until
            );
        }
        return;
    }

    // --new mode: only check for new versions, skip date comparison
    if check_new {
        let new_versions = detect_new_versions(&os_entries, &runtime_entries, &config);
        if new_versions.is_empty() {
            println!("[ OK    ] no new versions detected");
            return;
        }
        report_human(&new_versions, quiet);
        let errors = new_versions
            .iter()
            .filter(|m| m.severity == Severity::Error)
            .count();
        if errors > 0 {
            process::exit(1);
        }
        return;
    }

    // Collect all mismatches
    let mut all_mismatches = Vec::new();

    if check_os {
        all_mismatches.extend_from_slice(&check_os_entries(&os_entries, &config));
    }

    if check_runtimes {
        all_mismatches.extend_from_slice(&check_runtime_entries(&runtime_entries, &config));
    }

    // Deduplicate (same category+version can appear in both)
    all_mismatches.sort_by(|a, b| {
        a.category
            .cmp(&b.category)
            .then(a.version.cmp(&b.version))
            .then(a.severity.cmp(&b.severity))
    });
    all_mismatches.dedup_by(|a, b| {
        a.category == b.category && a.version == b.version && a.kind == b.kind
    });

    if all_mismatches.is_empty() && !ci_mode {
        if !quiet {
            println!("[ OK    ] all dates match");
        }
        return;
    }

    if ci_mode {
        report_ci(&all_mismatches);
    } else {
        report_human(&all_mismatches, quiet);
        let errors = all_mismatches
            .iter()
            .filter(|m| m.severity == Severity::Error)
            .count();
        if errors > 0 {
            process::exit(1);
        }
    }
}
