use std::{env, process};
use wmux_stress::{run_suite, StressProfile};

fn main() {
    let mut profile = StressProfile::Ci;
    let mut json = false;
    let mut args = env::args().skip(1);
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--profile" => {
                profile = match args.next().as_deref() {
                    Some("ci") => StressProfile::Ci,
                    Some("full") => StressProfile::Full,
                    Some(value) => exit_error(&format!("unknown stress profile {value:?}")),
                    None => exit_error("missing value for --profile"),
                };
            }
            "--json" => json = true,
            "--help" | "-h" => {
                println!("usage: wmux-stress [--profile ci|full] [--json]");
                return;
            }
            value => exit_error(&format!("unknown argument {value:?}")),
        }
    }

    let report = match run_suite(profile) {
        Ok(report) => report,
        Err(error) => exit_error(&error.to_string()),
    };
    if json {
        let cases = report
            .cases
            .iter()
            .map(|case| {
                format!(
                    "{{\"name\":\"{}\",\"operations\":{},\"fingerprint\":\"{:016x}\"}}",
                    case.name, case.operations, case.fingerprint
                )
            })
            .collect::<Vec<_>>()
            .join(",");
        println!(
            "{{\"profile\":\"{}\",\"cases\":[{}],\"suite_fingerprint\":\"{:016x}\"}}",
            report.profile.name(),
            cases,
            report.suite_fingerprint
        );
    } else {
        for case in &report.cases {
            println!(
                "{}: {} operations, {:016x}",
                case.name, case.operations, case.fingerprint
            );
        }
        println!(
            "stress-{}: {:016x}",
            report.profile.name(),
            report.suite_fingerprint
        );
    }
}

fn exit_error(message: &str) -> ! {
    eprintln!("wmux stress gate failed: {message}");
    process::exit(1)
}
