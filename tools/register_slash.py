#!/usr/bin/env python3
"""register_slash.py — register Discord application slash commands.

Reads .env (DISCORD_TOKEN, CLIENT_ID, GUILD_ID) and PUTs commands from slash.json.
Default is --dry-run (prints method + URL + command count, sends nothing).
With --send it performs the Discord REST call (needs network + real token).

Usage:
  python tools/register_slash.py [--env PATH] [--spec PATH] [--dry-run | --send]
"""
import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def parse_env(path: Path) -> dict:
    vals: dict = {}
    if not path.exists():
        return {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        vals[k.strip()] = v.strip().strip('"').strip("'")
    return vals


def main() -> int:
    args = sys.argv[1:]
    env_path = ROOT / ".env"
    if "--env" in args:
        env_path = Path(args[args.index("--env") + 1])
    if not env_path.exists():
        env_path = ROOT / ".env.example"

    spec_path = ROOT / "slash.json"
    if "--spec" in args:
        spec_path = Path(args[args.index("--spec") + 1])

    send = "--send" in args

    if not env_path.exists():
        print(f"missing environment file: {env_path}", file=sys.stderr)
        return 1

    vals = parse_env(env_path)
    token = vals.get("DISCORD_TOKEN", "")
    client = vals.get("CLIENT_ID", "")
    guild = vals.get("GUILD_ID", "")
    if not token or not client:
        print("need DISCORD_TOKEN + CLIENT_ID in .env", file=sys.stderr)
        return 1

    if not spec_path.exists():
        print(f"missing slash commands spec file: {spec_path}", file=sys.stderr)
        print("Create a slash.json file defining your commands (e.g. {\"commands\": [...]})", file=sys.stderr)
        return 1

    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    cmds = spec.get("commands", spec if isinstance(spec, list) else [])
    names = [c["name"] for c in cmds]
    assert len(names) == len(set(names)), "duplicate slash names"
    assert all(n == n.lower() for n in names), "slash names must be lowercase"

    scope = f"guild {guild}" if guild else "global"
    route = (f"/applications/{client}/guilds/{guild}/commands"
             if guild else f"/applications/{client}/commands")
    print(f"{len(cmds)} command(s) [{scope}]: {', '.join(names)}")
    print(f"PUT https://discord.com/api/v10{route}")
    if not send:
        print("dry-run: nothing sent (add --send to register)")
        return 0

    req = urllib.request.Request(
        f"https://discord.com/api/v10{route}",
        data=json.dumps(cmds).encode(),
        method="PUT",
        headers={"Authorization": f"Bot {token}",
                 "Content-Type": "application/json",
                 "User-Agent": "esp32-discord-bot-register/1.0"},
    )
    with urllib.request.urlopen(req, timeout=30) as res:
        body = json.loads(res.read().decode())
    print(f"registered {len(body)} command(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
