# KSNetFix - notes for Claude

## Permissions (granted by the repository owner, 2026-09-30)

- Claude may merge its own branches into `main` (open a pull request and merge it, or push
  to `main`) without asking first, once the `build` workflow passes on the branch.
- Never force-push `main`, rewrite its history, or delete tags / releases.

## Conventions

- Public repository: no decompiled game code, no game files.
- Version: `KSNETFIX_VERSION` in `src/ksnetfix.cpp`, plus the titles in `package/ZAINSTALUJ.bat`,
  `package/ODINSTALUJ.bat` and `package/INSTRUKCJA.txt`; release text in
  `docs/release-notes/v<version>.md`.
- A pushed tag `v<version>` runs `.github/workflows/release.yml`: MSVC build, `package.sh`, and a
  release with the full package (`dinput8.dll`, `steam_api.dll`, `ksnetfix.ini`, `INSTRUKCJA.txt`,
  `ZAINSTALUJ.bat`, `ODINSTALUJ.bat`). Tags without the `v` prefix do not trigger it.
- Cloud sessions cannot push tags (the session git proxy rejects them). Instead run `release.yml`
  by hand on `main` (workflow_dispatch, input `tag: v<version>`): it builds `main`, creates the
  tag on that commit and publishes the release.
