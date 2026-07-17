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
//!   1  — one or more errors found (wrong/expired dates, missed EOL, or a
//!        local failure such as an unreadable/invalid eol.json)
//!   2  — endoflife.date unreachable (every fetch failed) — neutral network
//!        outage only; reserved so CI can treat it as non-fatal flake

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
    supported_until: Option<String>,
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
    fetch_error: bool, // true = network/API failure, false = date mismatch or not-found
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
                    let Some(obj) = entry.as_object() else { continue };
                    os_entries.push(OsEntry {
                        category: category.clone(),
                        version: obj.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
                        eol: obj.get("eol").and_then(|v| v.as_str()).map(String::from),
                        supported_until: obj
                            .get("supported_until")
                            .and_then(|v| v.as_str())
                            .map(String::from),
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
                    let Some(obj) = entry.as_object() else { continue };
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
// Shared category mapping (matrix name → endoflife.date API name)
// ---------------------------------------------------------------------------

fn api_category<'a>(category: &'a str) -> Option<&'a str> {
    match category {
        "java" => Some("eclipse-temurin"),
        "node" => Some("nodejs"),
        "amazonlinux" => Some("amazon-linux"),
        "centos_stream" => Some("centos-stream"),
        "minimal" | "wasm" => None,
        // All other categories match their API name directly
        "go" | "perl" | "php" | "python" | "ruby"
        | "fedora" | "debian" | "ubuntu" | "alpine" | "rhel" => Some(category),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// HTTP fetch from endoflife.date (std::net::TcpStream only — zero deps)
// ---------------------------------------------------------------------------

fn fetch_api(category: &str) -> Result<String, String> {
    let url = format!("https://endoflife.date/api/{}.json", category);

    // Use curl (already installed in Docker image) — handles HTTPS + redirects
    let output = std::process::Command::new("curl")
        .args(["-fsL", "--max-time", "10", "-A", "unit-eol-check/0.1", &url])
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

/// Result of an API EOL lookup: Ok(Some(date)), Ok(None) (version not found in API),
/// or Err (network/parse failure).
fn api_eol_date(category: &str, version: &str) -> Result<Option<String>, String> {
    let api_cat = match api_category(category) {
        Some(c) => c,
        None => return Ok(None),
    };

    let api_json = fetch_api(api_cat)?;

    let entries: Vec<serde_json::Value> = serde_json::from_str(&api_json)
        .map_err(|e| format!("parse {} API JSON: {}", category, e))?;

    for entry in entries {
        let cycle = match entry.get("cycle").and_then(|v| v.as_str()) {
            Some(c) => c,
            None => continue,
        };
        if cycle == version {
            if let Some(eol_val) = entry.get("eol") {
                match eol_val {
                    serde_json::Value::String(s) => {
                        // Normalize to YYYY-MM (API may return YYYY-MM or YYYY-MM-DD)
                        if s.len() >= 7 {
                            return Ok(Some(s[..7].to_string()));
                        }
                    }
                    serde_json::Value::Bool(b) if !b => {
                        return Ok(Some(String::from("future")));
                    }
                    _ => {}
                }
            }
            return Ok(None);
        }
    }
    Ok(None)
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

    // Matrix categories to scan for new versions.
    // Must stay in sync with api_category() mapping — if a category is added
    // there, add it here too.
    let scan_cats: &[&str] = &[
        "go", "java", "node", "perl", "php", "python", "ruby",
        "fedora", "debian", "ubuntu", "alpine", "amazonlinux", "rhel", "centos_stream",
    ];

    for matrix_cat in scan_cats {
        let api_cat = match api_category(matrix_cat) {
            Some(c) => c,
            None => continue,
        };

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
                    fetch_error: false,
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
            // Local failure, not a network outage: exit 1 (real error), not 2.
            std::process::exit(1);
        })
}

fn check_os_entries(entries: &[OsEntry], _config: &Config) -> Vec<Mismatch> {
    let mut results = Vec::new();
    let now = now_yyyy_mm();
    let approaching_months = 12i64; // warn if EOL within 12 months

    for entry in entries {
        if let Some(matrix_date) = &entry.eol {
            if matrix_date == "future" {
                continue;
            }

            let actual = match api_eol_date(&entry.category, &entry.version) {
                Ok(v) => v,
                Err(e) => {
                    results.push(Mismatch {
                        category: entry.category.clone(),
                        version: entry.version.clone(),
                        kind: "os".to_string(),
                        matrix_date: Some(matrix_date.clone()),
                        actual_date: None,
                        severity: Severity::Warning,
                        fetch_error: true,
                        message: format!(
                            "could not fetch upstream EOL for {} {}: {}",
                            entry.category, entry.version, e
                        ),
                    });
                    continue;
                }
            };

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
                        fetch_error: false,
                        message: format!(
                            "upstream has no EOL date yet for {} {}",
                            entry.category, entry.version
                        ),
                    });
                }
                Some(actual_date) => {
                    let diff = match months_between(matrix_date, actual_date) {
                        Some(d) => d,
                        None => {
                            results.push(Mismatch {
                                category: entry.category.clone(),
                                version: entry.version.clone(),
                                kind: "os".to_string(),
                                matrix_date: Some(matrix_date.clone()),
                                actual_date: Some(actual_date.clone()),
                                severity: Severity::Error,
                                fetch_error: false,
                                message: format!(
                                    "{} {} cannot compare dates {} vs {}",
                                    entry.category, entry.version, matrix_date, actual_date
                                ),
                            });
                            continue;
                        }
                    };

                    if diff == 0 {
                        // Dates match — warn if EOL approaching
                        if let Some(months) = months_between(&now, matrix_date) {
                            if months <= approaching_months {
                                results.push(Mismatch {
                                    category: entry.category.clone(),
                                    version: entry.version.clone(),
                                    kind: "os".to_string(),
                                    matrix_date: Some(matrix_date.clone()),
                                    actual_date: Some(actual_date.clone()),
                                    severity: Severity::Warning,
                        fetch_error: false,
                                    message: format!(
                                        "{} {} EOL in ~{} months — plan migration",
                                        entry.category, entry.version, months
                                    ),
                                });
                            }
                        }
                    } else if diff < 0 {
                        // Matrix date is behind actual — update matrix
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "os".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            fetch_error: false,
                            message: format!(
                                "{} {} matrix EOL {} behind actual {} — update matrix",
                                entry.category, entry.version, matrix_date, actual_date
                            ),
                        });
                    } else {
                        // Matrix date is ahead of actual
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "os".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            fetch_error: false,
                            message: format!(
                                "{} {} matrix EOL {} ahead of actual {}",
                                entry.category, entry.version, matrix_date, actual_date
                            ),
                        });
                    }
                }
                None => {
                    // Version not found in API — warn
                    results.push(Mismatch {
                        category: entry.category.clone(),
                        version: entry.version.clone(),
                        kind: "os".to_string(),
                        matrix_date: Some(matrix_date.clone()),
                        actual_date: None,
                        severity: Severity::Warning,
                        fetch_error: false,
                        message: format!(
                            "version {} {} not found in upstream API",
                            entry.category, entry.version
                        ),
                    });
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

            let actual = match api_eol_date(&entry.category, &entry.version) {
                Ok(v) => v,
                Err(e) => {
                    results.push(Mismatch {
                        category: entry.category.clone(),
                        version: entry.version.clone(),
                        kind: "runtime".to_string(),
                        matrix_date: Some(matrix_date.clone()),
                        actual_date: None,
                        severity: Severity::Warning,
                        fetch_error: true,
                        message: format!(
                            "could not fetch upstream EOL for {} {}: {}",
                            entry.category, entry.version, e
                        ),
                    });
                    continue;
                }
            };

            match &actual {
                Some(actual_date) if actual_date == "future" => {
                    results.push(Mismatch {
                        category: entry.category.clone(),
                        version: entry.version.clone(),
                        kind: "runtime".to_string(),
                        matrix_date: Some(matrix_date.clone()),
                        actual_date: Some(actual_date.clone()),
                        severity: Severity::Info,
                        fetch_error: false,
                        message: format!(
                            "{} {} upstream EOL not yet set",
                            entry.category, entry.version
                        ),
                    });
                }
                Some(actual_date) => {
                    let diff_val = match months_between(matrix_date, actual_date) {
                        Some(d) => d,
                        None => {
                            results.push(Mismatch {
                                category: entry.category.clone(),
                                version: entry.version.clone(),
                                kind: "runtime".to_string(),
                                matrix_date: Some(matrix_date.clone()),
                                actual_date: Some(actual_date.clone()),
                                severity: Severity::Error,
                                fetch_error: false,
                                message: format!(
                                    "{} {} cannot compare dates {} vs {}",
                                    entry.category, entry.version, matrix_date, actual_date
                                ),
                            });
                            continue;
                        }
                    };

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
                                        fetch_error: false,
                                        message: format!(
                                            "{} {} upstream EOL passed ({}), add (EOL) flag",
                                            entry.category, entry.version, matrix_date
                                        ),
                                    });
                                }
                            }
                        }
                    } else if diff_val < 0 {
                        // Matrix behind actual
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "runtime".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            fetch_error: false,
                            message: format!(
                                "{} {} matrix EOL {} behind actual {}",
                                entry.category, entry.version, matrix_date, actual_date
                            ),
                        });
                    } else {
                        // Matrix ahead of actual
                        results.push(Mismatch {
                            category: entry.category.clone(),
                            version: entry.version.clone(),
                            kind: "runtime".to_string(),
                            matrix_date: Some(matrix_date.clone()),
                            actual_date: Some(actual_date.clone()),
                            severity: Severity::Error,
                            fetch_error: false,
                            message: format!(
                                "{} {} matrix EOL {} ahead of actual {}",
                                entry.category, entry.version, matrix_date, actual_date
                            ),
                        });
                    }
                }
                None => {
                    // Version not found in API — warn
                    results.push(Mismatch {
                        category: entry.category.clone(),
                        version: entry.version.clone(),
                        kind: "runtime".to_string(),
                        matrix_date: Some(matrix_date.clone()),
                        actual_date: None,
                        severity: Severity::Warning,
                        fetch_error: false,
                        message: format!(
                            "version {} {} not found in upstream API",
                            entry.category, entry.version
                        ),
                    });
                }
            }
        }
    }

    results
}

// ---------------------------------------------------------------------------
// Expiry enforcement gate
// ---------------------------------------------------------------------------

/// Push a `Severity::Error` mismatch when a still-shipped variant has outlived
/// its FreeUnit support window (`supported_until` strictly in the past).
///
/// The grace period is already baked into `supported_until` (`_grace_runtimes`
/// = 12mo, `_grace_os` = 36mo), so this is a category-agnostic date comparison:
/// once `supported_until < today` the variant is past EOL + grace and must be
/// dropped from the matrix. "Past upstream EOL but still within grace" stays a
/// WARN (handled in check_*_entries) — this gate fires only on a true breach.
fn push_if_expired(
    results: &mut Vec<Mismatch>,
    now: &str,
    category: &str,
    version: &str,
    kind: &str,
    supported_until: &Option<String>,
) {
    if version.is_empty() {
        return;
    }
    // A versioned, shipped entry with no `supported_until` can never be
    // expiry-checked — that's a data defect, not a pass. Fail the gate.
    let Some(sup_date) = supported_until else {
        results.push(Mismatch {
            category: category.to_string(),
            version: version.to_string(),
            kind: kind.to_string(),
            matrix_date: None,
            actual_date: Some(now.to_string()),
            severity: Severity::Error,
            fetch_error: false,
            message: format!(
                "{} {} has no supported_until — cannot enforce EOL + grace policy",
                category, version
            ),
        });
        return;
    };
    if sup_date == "future" {
        return;
    }
    // months_between(now, sup_date) < 0  ⇔  sup_date < now (strictly in the past).
    let Some(months) = months_between(now, sup_date) else {
        // Unparseable date on a versioned entry: another data defect the gate
        // must not skip silently.
        results.push(Mismatch {
            category: category.to_string(),
            version: version.to_string(),
            kind: kind.to_string(),
            matrix_date: Some(sup_date.clone()),
            actual_date: Some(now.to_string()),
            severity: Severity::Error,
            fetch_error: false,
            message: format!(
                "{} {} has unparseable supported_until {:?} — expected YYYY-MM",
                category, version, sup_date
            ),
        });
        return;
    };
    if months < 0 {
        results.push(Mismatch {
            category: category.to_string(),
            version: version.to_string(),
            kind: kind.to_string(),
            matrix_date: Some(sup_date.clone()),
            actual_date: Some(now.to_string()),
            severity: Severity::Error,
            fetch_error: false,
            message: format!(
                "{} {} outlived support window (supported_until {} < {}) — drop from matrix",
                category, version, sup_date, now
            ),
        });
    }
}

/// Enforcement gate: fail when any shipped runtime or OS variant is past its
/// `supported_until` date (EOL + grace). Offline — no API fetch required.
fn check_expired(
    os_entries: &[OsEntry],
    runtime_entries: &[RuntimeEntry],
    _config: &Config,
) -> Vec<Mismatch> {
    let mut results = Vec::new();
    let now = now_yyyy_mm();

    // Distinct kind ("*_expired") so the dedup in main() (keyed on
    // category+version+kind) never collapses this Error into a same-version
    // drift WARN from check_*_entries.
    for e in os_entries {
        push_if_expired(
            &mut results,
            &now,
            &e.category,
            &e.version,
            "os_expired",
            &e.supported_until,
        );
    }
    for e in runtime_entries {
        push_if_expired(
            &mut results,
            &now,
            &e.category,
            &e.version,
            "runtime_expired",
            &e.supported_until,
        );
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

            let actual = match api_eol_date(&entry.category, &entry.version) {
                Ok(v) => v,
                Err(err) => {
                    eprintln!(
                        "[ WARN  ] could not fetch EOL for {} {}: {}",
                        entry.category, entry.version, err
                    );
                    fixed.push(e);
                    continue;
                }
            };

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
            } else {
                eprintln!(
                    "[ WARN  ] {} {} not found in upstream API — skipping",
                    entry.category, entry.version
                );
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
            // A broken/unreadable eol.json is a real error (exit 1), NOT the
            // neutral network outage that exit 2 is reserved for — otherwise a
            // syntactically broken file would slip the PR gate green.
            process::exit(1);
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

    // Enforcement gate: fail on any variant past its supported_until (EOL +
    // grace). Offline check; honours --os / --runtimes scoping.
    let expiry_os: &[OsEntry] = if check_os { &os_entries } else { &[] };
    let expiry_rt: &[RuntimeEntry] = if check_runtimes { &runtime_entries } else { &[] };
    all_mismatches.extend(check_expired(expiry_os, expiry_rt, &config));

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

    // If all mismatches are fetch errors (network/API failure), exit 2
    let fetch_failures = all_mismatches
        .iter()
        .filter(|m| m.fetch_error)
        .count();
    if fetch_failures > 0 && fetch_failures == all_mismatches.len() {
        eprintln!(
            "[ ERROR ] all {} entries failed to fetch — network or API error",
            fetch_failures
        );
        process::exit(2);
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

#[cfg(test)]
mod tests {
    use super::*;

    fn run_gate(now: &str, sup: Option<&str>, version: &str) -> Vec<Mismatch> {
        let mut results = Vec::new();
        push_if_expired(
            &mut results,
            now,
            "debian",
            version,
            "os_expired",
            &sup.map(String::from),
        );
        results
    }

    #[test]
    fn expired_supported_until_is_hard_error() {
        let r = run_gate("2026-07", Some("2025-09"), "10");
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].severity, Severity::Error);
        assert_eq!(r[0].kind, "os_expired");
        assert!(!r[0].fetch_error);
    }

    #[test]
    fn expiry_fires_the_month_after_supported_until() {
        // supported_until == current month -> still supported (not expired) ...
        assert!(run_gate("2026-07", Some("2026-07"), "12").is_empty());
        // ... and one month later it is a breach.
        assert_eq!(run_gate("2026-08", Some("2026-07"), "12").len(), 1);
    }

    #[test]
    fn future_supported_until_stays_silent() {
        // In-grace / future dates are not this gate's business: the grace
        // window WARN comes from check_*_entries, never an Error from here.
        assert!(run_gate("2026-07", Some("2029-07"), "12").is_empty());
        assert!(run_gate("2026-07", Some("future"), "12").is_empty());
    }

    #[test]
    fn empty_version_is_skipped() {
        // A reference/aggregate row with no version is not a shipped variant.
        assert!(run_gate("2026-07", Some("2020-01"), "").is_empty());
        assert!(run_gate("2026-07", None, "").is_empty());
    }

    #[test]
    fn missing_supported_until_on_versioned_entry_is_error() {
        // A versioned, shipped entry that can't be expiry-checked is a data
        // defect the gate must catch, not silently pass.
        let r = run_gate("2026-07", None, "12");
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].severity, Severity::Error);
        assert!(r[0].message.contains("no supported_until"));
    }

    #[test]
    fn unparseable_supported_until_is_error() {
        let r = run_gate("2026-07", Some("n/a"), "12");
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].severity, Severity::Error);
        assert!(r[0].message.contains("unparseable"));
    }
}
