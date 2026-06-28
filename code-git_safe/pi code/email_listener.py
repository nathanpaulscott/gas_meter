#!/usr/bin/env python3
import imaplib, email, time, os
from email.header import decode_header
from datetime import datetime

# ==============================
# CONFIG
# ==============================
DEBUG = True

EMAIL_ACCOUNT  = "nathanpaulscott@yahoo.com"
PASSWORD_FILE  = "/home/pi/gasmon/yp.sec"
IMAP_SERVER    = "imap.mail.yahoo.com"
IMAP_PORT      = 993

TRUSTED_SENDER   = "nathan.scott.rf@gmail.com"
COMMAND_SUBJECT  = "GAS_COMMAND"

POLL_SECONDS = 3600

# Log files (capped)
LOG_FILE = "/home/pi/gasmon/email_listener.log"
ERR_FILE = "/home/pi/gasmon/email_listener.err"
MAX_LINES = 100  # cap log length


# ==============================
# LOGGING HELPERS
# ==============================
def write_log(path, text):
    """Append a line to log file but keep only last MAX_LINES."""
    try:
        with open(path, "r") as f:
            lines = f.readlines()
    except:
        lines = []

    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines.append(f"{timestamp} {text}\n")

    # keep last MAX_LINES
    lines = lines[-MAX_LINES:]

    with open(path, "w") as f:
        f.writelines(lines)


def debug(msg):
    if DEBUG:
        print(msg)
        write_log(LOG_FILE, msg)


def err(msg):
    print("[ERROR]", msg)
    write_log(ERR_FILE, msg)


# ==============================
# LOAD PASSWORD
# ==============================
with open(PASSWORD_FILE, "r") as f:
    EMAIL_PASSWORD = f.read().strip()


# ==============================
# IMAP LOGIN
# ==============================
def connect_imap():
    while True:
        try:
            debug("[DEBUG] Connecting to IMAP...")
            M = imaplib.IMAP4_SSL(IMAP_SERVER, IMAP_PORT)
            M.login(EMAIL_ACCOUNT, EMAIL_PASSWORD)
            debug("[DEBUG] IMAP login OK")
            return M
        except Exception as e:
            err(f"IMAP CONNECT ERROR: {e}")
            time.sleep(15)


# ==============================
# SUBJECT DECODING
# ==============================
def decode_subj(raw_subj):
    if raw_subj is None:
        return ""
    decoded, charset = decode_header(raw_subj)[0]
    if isinstance(decoded, bytes):
        return decoded.decode(charset or "utf-8", errors="ignore")
    return decoded


# ==============================
# CHECK FOR NEW COMMAND EMAILS
# ==============================
def check_for_commands(M):
    M.select("INBOX")

    status, data = M.search(None, "(UNSEEN)")
    if status != "OK":
        debug("[DEBUG] SEARCH UNSEEN failed")
        return []

    all_ids = data[0].split()
    total_unseen = len(all_ids)
    debug(f"[DEBUG] Total UNSEEN messages: {total_unseen}")

    if total_unseen == 0:
        return []

    # Check only last 10 unseen
    ids_to_check = all_ids[-10:]
    debug(f"[DEBUG] Checking last {len(ids_to_check)} UNSEEN messages")

    commands = []

    for msg_id in reversed(ids_to_check):  # newest first
        debug(f"[DEBUG] Fetching msg_id {msg_id.decode('ascii', 'ignore')}")

        status, msg_data = M.fetch(msg_id, "(RFC822)")
        if status != "OK":
            debug("[DEBUG] FETCH failed")
            continue

        msg = email.message_from_bytes(msg_data[0][1])
        subj = decode_subj(msg.get("Subject"))

        debug(f"[DEBUG] Subject: {subj}")

        # Subject filter
        if subj.strip().upper() != COMMAND_SUBJECT:
            debug("[DEBUG] Skipping (subject mismatch)")
            continue

        # Sender filter
        sender = (msg.get("From") or "").lower()
        if TRUSTED_SENDER.lower() not in sender:
            debug(f"[DEBUG] Skipping (sender mismatch: {sender})")
            continue

        debug(f"[DEBUG] Matched command email from {sender}")

        # Extract body
        body = ""
        if msg.is_multipart():
            for part in msg.walk():
                if part.get_content_type() == "text/plain":
                    body_bytes = part.get_payload(decode=True)
                    if body_bytes:
                        body = body_bytes.decode("utf-8", errors="ignore")
                    break
        else:
            body_bytes = msg.get_payload(decode=True)
            if body_bytes:
                body = body_bytes.decode("utf-8", errors="ignore")

        body = body.strip()
        debug(f"[DEBUG] Body: {repr(body)}")

        commands.append((msg_id, body))

    return commands


# ==============================
# HANDLE COMMAND
# ==============================
def handle_command(body_text):
    debug(f"COMMAND RECEIVED: {body_text}")

    tokens = body_text.split()
    if not tokens:
        err("Empty command")
        return

    cmd = tokens[0].lower()

    if cmd == "plot" and len(tokens) >= 2 and tokens[1].lower() == "last48":
        command = "python3 /home/pi/gasmon/gas_plot_rate.py last48"
        debug(f"Executing: {command}")
        os.system(command)
        return

    if cmd == "plot" and len(tokens) >= 2 and tokens[1].lower() == "last30":
        command = "python3 /home/pi/gasmon/gas_plot_rate.py last30"
        debug(f"Executing: {command}")
        os.system(command)
        return

    if cmd == "plot" and len(tokens) == 3:
        d1, d2 = tokens[1], tokens[2]
        command = f"python3 /home/pi/gasmon/gas_plot_rate.py {d1} {d2}"
        debug(f"Executing: {command}")
        os.system(command)
        return

    # ========= rawdata (new feature) =========
    if cmd == "rawdata" and len(tokens) == 3:
        d1, d2 = tokens[1], tokens[2]
        command = f"python3 /home/pi/gasmon/rawdata_dump.py {d1} {d2}"
        debug(f"Executing: {command}")
        os.system(command)
        return

    if cmd == "reboot":
        debug("Rebooting system by command...")
        os.system("sudo reboot")
        return

    if cmd == "stop":
        debug("Stopping listener by command...")
        exit(0)

    if cmd == "sendlogs":
        debug("Sending logs to user...")
        os.system("python3 /home/pi/gasmon/send_logs.py")
        return

    err("UNKNOWN COMMAND FORMAT")


# ==============================
# MARK AS SEEN
# ==============================
def mark_as_seen(M, msg_id):
    debug(f"[DEBUG] Marking msg_id {msg_id.decode('ascii','ignore')} as SEEN")
    M.store(msg_id, "+FLAGS", "\\Seen")


# ==============================
# MAIN LOOP
# ==============================
def main_loop():
    while True:
        debug("\n[DEBUG] === New poll cycle ===")
        M = connect_imap()
        cmds = check_for_commands(M)

        debug(f"[DEBUG] Found {len(cmds)} command(s) this cycle")

        for msg_id, body in cmds:
            handle_command(body)
            mark_as_seen(M, msg_id)
            break  # only handle the first valid command

        M.logout()
        debug(f"[DEBUG] Sleeping {POLL_SECONDS} s before next poll")
        time.sleep(POLL_SECONDS)


# ==============================
# RUN
# ==============================
if __name__ == "__main__":
    main_loop()