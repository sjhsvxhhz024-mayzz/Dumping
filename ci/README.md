# CI workflow — manual staging directory

The file  ndk-build.yml  in this directory is the intended GitHub Actions
workflow for building the arm64-v8a binary on every push and PR.

It lives here (not under  .github/workflows/) because the GitHub App
integration used by the ai-agent lacks the  Workflows: Read and write
permission — pushing to any path under  .github/workflows/  returns
403 Resource not accessible by integration.

## To activate

Locally:

    git mv ci/ndk-build.yml .github/workflows/ndk-build.yml
    git commit -m "chore(ci): activate ndk-build workflow"
    git push

Or grant the app the workflows scope in
Settings → Integrations → GitHub → Configure, then re-push this file
directly into .github/workflows/.

## What it does

1. Checkout code, JDK 17, install NDK r27.
2. Run  Nov/test_typeinfo_offline.py  (39 / 39 asserts, ~1 s).
3. cd proj/ &&  ndk-build -j2 NDK_APPLICATION_MK=Application.mk
4. Upload  proj/libs/arm64-v8a/*  as artifact
   eclipsoxide-arm64-v8a-<sha> ( 30d retention ).

Concurrency group cancels superseded runs on the same ref, so a rapid
sequence of pushes only builds the latest.
