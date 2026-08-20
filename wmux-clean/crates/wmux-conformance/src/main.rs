use std::process;

fn main() {
    let report = wmux_conformance::verify_portable_suite().unwrap_or_else(|error| {
        eprintln!("conformance failed: {error}");
        process::exit(1);
    });
    println!("platform={}", std::env::consts::OS);
    for case in report.cases {
        println!("{}={:016x}", case.name, case.fingerprint);
    }
    println!("suite={:016x}", report.suite_fingerprint);
}
