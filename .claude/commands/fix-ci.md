Diagnose and fix failing CI workflows.

Steps:
1. Check the latest CI run status: `gh run list --limit 5`
2. If there's a failing run, get its details: `gh run view <run-id> --log-failed`
3. Analyze the failure - common issues:
   - Missing dependencies in the CI environment
   - Platform-specific build failures (Linux/Windows/macOS)
   - Node.js version deprecation in GitHub Actions
   - Qt module availability differences across platforms
4. Fix the issue in the relevant file (usually `.github/workflows/build.yml`)
5. Build locally to verify the fix doesn't break the Linux build
6. Show the diff for review

Notes:
- The Windows build uses vcpkg with packages at `C:\vcpkg\installed\x64-windows`
- macOS build uses Homebrew (`brew install qt@5 portaudio opus openssl@3`)
- `pttyhandler` is POSIX-only; Windows uses a no-op stub via `#ifdef Q_OS_WIN`
