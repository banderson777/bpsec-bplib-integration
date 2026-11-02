# Download, build and run

The supported quick-start environment is Ubuntu 24.04 x86-64, including Ubuntu
24.04 running in WSL2 on Windows. The first build downloads Python packages and
compiles dependencies; later builds reuse their generated files.

## Windows

Open PowerShell and, if Ubuntu/WSL is not already installed, run:

```powershell
wsl --install -d Ubuntu-24.04
```

Finish Ubuntu's first-run setup and open its terminal. Run the remaining commands
inside Ubuntu. Keep the checkout in your Linux home directory at a path without
spaces; generated Linux build caches are specific to that checkout.

## Install system packages

```bash
sudo apt-get update
sudo apt-get install -y git build-essential cmake ninja-build pkg-config rsync \
  libssl-dev libjansson-dev libsqlite3-dev python3 python3-venv unzip
```

The build requires CMake >=3.23, OpenSSL >=3.0, Jansson >=2.14 and SQLite >=3.31.1.
Ubuntu 24.04 supplies compatible development packages. The script checks these
requirements before starting. Ubuntu 22.04's default Jansson package is older;
use the supported quick-start environment or arrange the required packages manually.

## Get all source and build

```bash
mkdir -p ~/src
cd ~/src
git clone --recurse-submodules https://github.com/banderson777/bpsec-bplib-integration.git
cd bpsec-bplib-integration
bash scripts/bootstrap.sh --native-test
```

This builds QCBOR/m-lib/Unity staging, BSL, the native cFS mission and the integration
project. It runs the integration tests and a real five-process transfer, verifying
matching output files and custody drain. It creates a Python virtual environment
and installs `client-test/requirements.txt`; no global pip installation is needed.
The initial source snapshot has 37 integration CTests. Test counts can change as
the project develops; the current CTest output and CI run are authoritative.

For an existing clone with missing submodule files:

```bash
git submodule update --init --recursive
```

For a complete-source release ZIP, extract it inside Ubuntu with `unzip`, enter
the extracted `bpsec-bplib-integration` directory and run the same bootstrap command.
The complete-source asset includes dependency source and a commit/hash manifest;
GitHub's ordinary green-button ZIP omits submodule contents. The bootstrap script
works without Git metadata when all dependency source is present.

## Build options and output

```bash
bash scripts/bootstrap.sh --check        # Check tools, package versions and sources
bash scripts/bootstrap.sh               # Build and run integration tests
bash scripts/bootstrap.sh --build-only  # Build without running tests
```

Builds live under `build/native/`, test runtime files under `build/runs/`, and Python
under `.venv/`. Run the scripts with `.venv/bin/python` or activate the environment
with `source .venv/bin/activate`. To repeat the native baseline, use a new run path:

```bash
.venv/bin/python demo/test_five_node.py \
  --build-dir "$PWD/build/native" \
  --run-dir "$PWD/build/runs/my-baseline" --port-base 34000 --timeout 90
```

Only run one native lab instance at a time. The automated baseline generates its
own small test segments; FFmpeg is needed only for preparing real video or playback.
The upstream BSL unit suite is separate from the integration suite. The quick-start
build disables BSL's unit/memcheck tools; see docs/build-and-test.md for its full
development workflow.

GitHub Actions runs the quick-start build/tests and archives validation logs plus
complete source. A public release can attach the generated complete-source ZIP so
people can download dependency source without using Git.

See [build details](build-and-test.md) and the [five-node runbook](five-node-video-test.md)
for architecture, fault scenarios and the limits of existing validation.
