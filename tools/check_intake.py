#!/usr/bin/env python3
import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

UPSTREAM_DEFAULT = "boykopovar/AnyPS5"
AUTHOR_DEFAULT = "ivica00"

def find_gh():
    found = shutil.which("gh")
    if found:
        return found
    standard = "C:\\Program Files\\GitHub CLI\\gh.exe"
    if os.path.exists(standard):
        return standard
    return None

def fetch_upstream_file(repo, path):
    gh = find_gh()
    if gh:
        try:
            cmd = [gh, "api", f"repos/{repo}/contents/{path}"]
            proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
            data = json.loads(proc.stdout)
            if "content" in data:
                return base64.b64decode(data["content"]).decode("utf-8", errors="replace")
        except Exception:
            pass
    url = f"https://raw.githubusercontent.com/{repo}/main/{path}"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "AnyPS5-Intake-Monitor"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as err:
        if err.code == 404:
            return None
        raise
    except Exception:
        return None

def check_intake_status(repo):
    content = fetch_upstream_file(repo, ".github/workflows/pr-intake.yml")
    if content is None:
        return {
            "paused": False,
            "threshold": 0,
            "reason": "pr-intake.yml does not exist on upstream (intake is OPEN)"
        }
    match = re.search(r"minimumMergedPRs\s*=\s*(\d+)", content)
    if not match:
        return {
            "paused": False,
            "threshold": 0,
            "reason": "minimumMergedPRs variable removed from workflow (intake is OPEN)"
        }
    threshold = int(match.group(1))
    if threshold <= 0:
        return {
            "paused": False,
            "threshold": threshold,
            "reason": f"threshold lowered to {threshold} (intake is OPEN)"
        }
    return {
        "paused": True,
        "threshold": threshold,
        "reason": f"intake paused: requires {threshold} merged PRs"
    }

def fetch_user_prs(repo, author):
    gh = find_gh()
    if not gh:
        return []
    try:
        cmd = [gh, "pr", "list", "--repo", repo, "--state", "all", "--author", author, "--json", "number,title,state,url"]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return json.loads(proc.stdout)
    except Exception:
        return []

def search_carry_prs(repo, pr_number):
    gh = find_gh()
    if not gh:
        return []
    try:
        cmd = [gh, "pr", "list", "--repo", repo, "--state", "all", "--search", f"carry-{pr_number}", "--json", "number,title,state,url"]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return json.loads(proc.stdout)
    except Exception:
        return []

def reopen_pr(repo, pr_number):
    gh = find_gh()
    if not gh:
        return False
    try:
        cmd = [gh, "pr", "reopen", str(pr_number), "--repo", repo]
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        return True
    except Exception:
        return False

def show_notification(title, message):
    if sys.platform != "win32":
        return
    escaped_title = title.replace('"', '`"')
    escaped_msg = message.replace('"', '`"')
    ps_cmd = f"""
    [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null
    $template = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02)
    $textNodes = $template.GetElementsByTagName("text")
    $textNodes.Item(0).AppendChild($template.CreateTextNode("{escaped_title}")) > $null
    $textNodes.Item(1).AppendChild($template.CreateTextNode("{escaped_msg}")) > $null
    $notifier = [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("AnyPS5 Intake Monitor")
    $toast = [Windows.UI.Notifications.ToastNotification]::new($template)
    $notifier.Show($toast)
    """
    try:
        subprocess.run(["powershell", "-NoProfile", "-Command", ps_cmd], capture_output=True)
    except Exception:
        pass

def main():
    parser = argparse.ArgumentParser(description="Monitor PR intake status on AnyPS5 upstream")
    parser.add_argument("--repo", default=UPSTREAM_DEFAULT, help="Upstream repository")
    parser.add_argument("--author", default=AUTHOR_DEFAULT, help="PR author username")
    parser.add_argument("--notify", action="store_true", help="Send Windows desktop notification if open or carry found")
    parser.add_argument("--json", dest="as_json", action="store_true", help="Output JSON")
    parser.add_argument("--reopen", action="store_true", help="Automatically reopen closed PRs if intake is open")
    args = parser.parse_args()

    intake = check_intake_status(args.repo)
    prs = fetch_user_prs(args.repo, args.author)
    carries = {}
    for pr in prs:
        found_carries = search_carry_prs(args.repo, pr["number"])
        if found_carries:
            carries[pr["number"]] = found_carries

    report = {
        "repo": args.repo,
        "author": args.author,
        "intake": intake,
        "prs": prs,
        "carries": carries,
        "action_required": (not intake["paused"]) or bool(carries)
    }

    if args.as_json:
        print(json.dumps(report, indent=2))
        return 10 if report["action_required"] else 0

    print("=" * 60)
    print(f" AnyPS5 Upstream Intake Monitor ({args.repo})")
    print("=" * 60)
    if intake["paused"]:
        print(f"[*] Intake status: PAUSED ({intake['reason']})")
    else:
        print(f"[!] Intake status: OPEN! ({intake['reason']})")

    print(f"\n[*] Tracked PRs for @{args.author}:")
    if not prs:
        print("    (no PRs found)")
    for pr in prs:
        state_symbol = "CLOSED" if pr["state"] == "CLOSED" else "OPEN"
        print(f"    - PR #{pr['number']}: [{state_symbol}] {pr['title']}")
        print(f"      URL: {pr['url']}")
        if pr["number"] in carries:
            print("      >>> CARRY PR FOUND:")
            for c in carries[pr["number"]]:
                print(f"          - #{c['number']}: {c['title']} ({c['url']})")

    if report["action_required"]:
        msg = "Intake is open or a carry PR was discovered!"
        print(f"\n[!] ACTION REQUIRED: {msg}")
        if args.notify:
            show_notification("AnyPS5 Intake Update", msg)
        if args.reopen and not intake["paused"]:
            for pr in prs:
                if pr["state"] == "CLOSED":
                    ok = reopen_pr(args.repo, pr["number"])
                    print(f"    Reopening PR #{pr['number']}: {'SUCCESS' if ok else 'FAILED'}")
        return 10

    print("\n[-] No action needed. Upstream intake is still paused.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
