# DOCA Project

This repository contains DOCA-based application code developed on macOS and validated in Linux containers before being tested on the target in-house environment.

## Development flow

1. Write code on macOS with Codex.
2. Build-check in Docker for amd64 and arm64.
3. Push to GitHub.
4. Pull and validate on the in-house DOCA runtime environment.
5. Push logs/fixes back and repeat.

## SDK layout

Expected DOCA SDK root inside containers:

```text
/opt/mellanox/doca
```
