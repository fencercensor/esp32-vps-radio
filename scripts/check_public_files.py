#!/usr/bin/env python3
"""Check tracked public files without printing potentially sensitive values."""
import ipaddress
from pathlib import Path
import re
import subprocess
import sys

root = Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip())
tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0')
private = {
    'firmware/radio_config.h', 'server/data/admin.json',
    'server/data/radio-config.json', 'server/bin/ffmpeg',
}
allowed_ips = {'127.0.0.1', '192.168.4.1', '192.0.2.1'}
secret = re.compile(
    r'BEGIN\s+(?:RSA |OPENSSH |EC |DSA )?PRIVATE KEY|'
    r'gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,}|'
    r'AKIA[0-9A-Z]{16}|sk-[A-Za-z0-9]{20,}'
)
personal_path = re.compile(r'/(?:Users/|home/[A-Za-z0-9._-]+/)')
ipv4 = re.compile(r'(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])')
private_url = re.compile(r'https?://[^\s/]+:[^\s/]+@|[?&](?:token|api_key|apikey|signature|password|secret)=', re.I)
arabic = re.compile(r'[\u0600-\u06ff]')
errors = []
for name in filter(None, tracked):
    path = root / name
    if name in private or path.suffix in {'.bin', '.elf', '.map', '.o', '.log', '.tgz', '.zip'}:
        errors.append((name, 'private runtime file, binary, log or backup'))
    if not path.is_file() or path.suffix in {'.png', '.pdf'}:
        continue
    try:
        body = path.read_text()
    except UnicodeDecodeError:
        errors.append((name, 'unexpected binary file'))
        continue
    for pattern, reason in [(secret, 'possible secret'), (personal_path, 'personal absolute path'),
                            (private_url, 'credential or signed URL'), (arabic, 'non-English Arabic text')]:
        if pattern.search(body):
            errors.append((name, reason))
    for match in ipv4.findall(body):
        try:
            ipaddress.ip_address(match)
        except ValueError:
            continue
        if match not in allowed_ips:
            errors.append((name, 'non-example IP address'))
if errors:
    for name, reason in errors:
        print(f'ERROR: {name}: {reason}', file=sys.stderr)
    sys.exit(1)
print('Security check passed: reviewed public file types, secret patterns, paths, addresses and English text.')
