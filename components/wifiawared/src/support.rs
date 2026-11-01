use anyhow::{Result, ensure};
use std::{io::Write, net::Ipv6Addr, os::unix::fs::OpenOptionsExt, path::Path};

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn unhex(text: &str, size: usize) -> Result<Vec<u8>> {
    ensure!(
        text.len() == size * 2 && text.bytes().all(|b| b.is_ascii_hexdigit()),
        "十六进制字段长度或内容无效"
    );
    (0..text.len())
        .step_by(2)
        .map(|i| Ok(u8::from_str_radix(&text[i..i + 2], 16)?))
        .collect()
}

pub fn field<'a>(text: &'a str, name: &str) -> Option<&'a str> {
    let needle = format!("{name}=");
    text.match_indices(&needle).find_map(|(offset, _)| {
        if offset > 0
            && (text.as_bytes()[offset - 1].is_ascii_alphanumeric()
                || text.as_bytes()[offset - 1] == b'_')
        {
            return None;
        }
        text[offset + needle.len()..]
            .split([' ', '\t', '\r', '\n', ','])
            .next()
            .filter(|s| !s.is_empty())
    })
}

pub fn link_local(mac: &str, interface_id: Option<&str>) -> Result<String> {
    let mut bytes = [0u8; 16];
    bytes[0] = 0xfe;
    bytes[1] = 0x80;
    if let Some(id) = interface_id.filter(|s| !s.is_empty()) {
        bytes[8..].copy_from_slice(&unhex(id, 8)?);
    } else {
        let mac = unhex(&mac.trim().replace(':', ""), 6)?;
        bytes[8..].copy_from_slice(&[
            mac[0] ^ 2,
            mac[1],
            mac[2],
            0xff,
            0xfe,
            mac[3],
            mac[4],
            mac[5],
        ]);
    }
    Ok(Ipv6Addr::from(bytes).to_string())
}

pub fn write_configuration(path: &Path, contents: &str, exclusive: bool) -> Result<()> {
    let file = std::fs::OpenOptions::new()
        .write(true)
        .create(!exclusive)
        .create_new(exclusive)
        .truncate(!exclusive)
        .mode(0o600)
        .open(path);
    let mut file = match file {
        Err(e) if exclusive && e.kind() == std::io::ErrorKind::AlreadyExists => return Ok(()),
        other => other?,
    };
    file.write_all(contents.as_bytes())?;
    if exclusive {
        file.sync_all()?;
    }
    Ok(())
}

pub fn redact(line: &str) -> String {
    let mut bytes = line.as_bytes().to_vec();
    let lower = line.to_ascii_lowercase();
    for name in ["nd_pmk=", "pmk=", "nik=", "pwd_hex="] {
        for (index, _) in lower.match_indices(name) {
            if index > 0 && (bytes[index - 1].is_ascii_alphanumeric() || bytes[index - 1] == b'_') {
                continue;
            }
            for b in &mut bytes[index + name.len()..] {
                if b.is_ascii_whitespace() {
                    break;
                }
                *b = b'*';
            }
        }
    }
    String::from_utf8_lossy(&bytes).into_owned()
}
