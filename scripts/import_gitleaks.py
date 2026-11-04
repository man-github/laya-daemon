#!/usr/bin/env python3
"""
Laya AI Gateway - Gitleaks Rulebook Importer
Downloads official gitleaks.toml from GitHub and converts rules into Laya's patterns.json format.
"""
import urllib.request
import tomllib
import json
import re
import sys
import os

GITLEAKS_URL = "https://raw.githubusercontent.com/gitleaks/gitleaks/master/config/gitleaks.toml"

def main():
    dest_file = sys.argv[1] if len(sys.argv) > 1 else "config/patterns_gitleaks.json"
    print(f"Fetching official Gitleaks rules from: {GITLEAKS_URL}")

    req = urllib.request.Request(GITLEAKS_URL, headers={"User-Agent": "Laya-Rule-Importer/1.0"})
    with urllib.request.urlopen(req) as resp:
        content = resp.read()

    data = tomllib.loads(content.decode("utf-8"))
    rules = data.get("rules", [])
    print(f"Parsed {len(rules)} rules from gitleaks.toml")

    patterns = []
    for r in rules:
        rule_id = r.get("id", "").upper().replace("-", "_")
        regex = r.get("regex", "")
        desc = r.get("description", "")
        if not rule_id or not regex:
            continue

        # Standardize named capture groups (?P<name>...) to standard capture groups for Google RE2
        clean_regex = re.sub(r'\(\?P<[a-zA-Z0-9_]+>', '(', regex)

        patterns.append({
            "name": rule_id,
            "regex": clean_regex,
            "verdict": "SECRET_LEAK",
            "score": 99,
            "description": desc
        })

    out_data = {
        "policy_epoch": 1,
        "description": "Enterprise DLP Secret Rules imported from Gitleaks",
        "total_rules": len(patterns),
        "patterns": patterns
    }

    os.makedirs(os.path.dirname(os.path.abspath(dest_file)), exist_ok=True)
    with open(dest_file, "w", encoding="utf-8") as f:
        json.dump(out_data, f, indent=2)

    print(f"Successfully generated {len(patterns)} Laya DLP patterns in: {dest_file}")

if __name__ == "__main__":
    main()
