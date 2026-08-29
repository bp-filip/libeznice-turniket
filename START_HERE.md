# Start here

This project was developed in a chat session and is being handed to Claude
Code. Delete this file once the first commit is done.

## Paste this to Claude Code as the first message

> This directory holds firmware for a QR-code turnstile, developed so far in
> a chat session and never committed. Read `CLAUDE.md` first — it has the
> hardware constraints and known issues.
>
> Please:
> 1. `git init` if this is not already a repository, or confirm the remote
>    if it is (`bp-filip/libeznice-turniket`).
> 2. Confirm `.gitignore` covers `secrets.h` and verify with
>    `git status --ignored` that `esp32_turnstile/secrets.h` is not staged.
>    It holds live WiFi and OTA passwords and the repository is public.
> 3. Create a branch `chat-handoff` and commit everything on it with the
>    message "Import turnstile firmware from chat session".
> 4. Show me the diff summary and confirm nothing secret was committed.
>
> Do not push. I will review the branch first.

## Before running that

If the GitHub repository already has history, `git init` here will not match
it. In that case clone the repository separately and copy these files in, so
the branch has the real history behind it.

Check whether credentials were committed in the past. If `secrets.h` was
never used before now, the previous sketch had the WiFi and OTA passwords
inline, and they may be sitting in the existing history:

```
git log --all -S 'Ab123456!' --oneline
```

Anything found there needs the passwords rotated — removing the file from a
later commit does not remove it from history, and the repository is public.

## Suggested first tasks after the commit

Ordered by value, taken from the "known rough edges" list in `CLAUDE.md`:

1. **Move the idle and busy text onto the Nano.** Currently the ESP32
   retransmits them every 12 seconds to beat the Nano's fallback, which
   repaints the screen visibly. Putting the Czech strings in `IDLE_TEXT` and
   `BUSY_TEXT` with 20 leading spaces removes the whole mechanism.
2. **Turn TLS validation back on.** `setInsecure()` is used everywhere. Add
   `configTime(0, 0, "pool.ntp.org")` and a clock guard before the first
   update check — an unset clock is the most likely reason certificate
   validation failed originally.
3. **Fix the level shifter.** Software now defends against the crosstalk,
   but the hardware fault is still there.
4. **Decide what `ALLOW_MULTI` should do.** It is implemented as a 5-minute
   hold but has never been tested, and the panic motor it was designed
   around is broken.
