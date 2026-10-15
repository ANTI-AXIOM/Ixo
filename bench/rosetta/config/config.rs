// Reading settings with every failure handled: a missing file, bad lines, bad numbers.
use std::fs;

enum Value {
    Int(i64),
    Text(String),
}

fn load(path: &str) -> std::io::Result<(Vec<(String, Value)>, Vec<String>)> {
    let numeric = ["port", "workers", "timeout"];
    let (mut settings, mut errors) = (Vec::new(), Vec::new());
    for (i, line) in fs::read_to_string(path)?.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            errors.push(format!("line {}: expected key = value", i + 1));
            continue;
        };
        let (key, value) = (key.trim(), value.trim());
        if !numeric.contains(&key) {
            settings.push((key.to_string(), Value::Text(value.to_string())));
            continue;
        }
        match value.parse() {
            Ok(n) => settings.push((key.to_string(), Value::Int(n))),
            Err(_) => errors.push(format!("line {}: {} is not a number: {}", i + 1, key, value)),
        }
    }
    Ok((settings, errors))
}

fn main() -> std::io::Result<()> {
    let path = "settings.conf";
    fs::write(path, "# server settings\nport = 8080\nhost = example.org\nworkers = many\ntimeout = 30\nbad line\n")?;
    for name in ["missing.conf", path] {
        let Ok((settings, errors)) = load(name) else {
            println!("{}: using defaults", name);
            continue;
        };
        for (k, v) in settings {
            match v {
                Value::Int(n) => println!("{} {}", k, n),
                Value::Text(s) => println!("{} {}", k, s),
            }
        }
        for e in errors {
            println!("error: {}", e);
        }
    }
    fs::remove_file(path)
}
