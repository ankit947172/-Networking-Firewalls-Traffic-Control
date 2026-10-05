# VM setup, troubleshooting, and GitHub submission

## VM preparation

Use an Ubuntu Linux VM with approximately 2 virtual CPUs, 2 GB RAM, and space
for a compiler toolchain. NAT is sufficient. Take a snapshot, copy the ZIP to
the VM, extract it under your home directory, and open a Linux terminal.
No kernel rebuild, Secure Boot change, host OS replacement, or custom module
installation is required by the program.

```sh
cd Domain3-Traffic-Control-Capstone
sudo apt-get update
sudo apt-get install -y g++ cmake make
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
ctest --test-dir build --output-on-failure
sudo ./build/traffic_lab self-test
```

Only network-lab execution needs privileges; compilation/unit tests do not.
Testing during development used Ubuntu WSL2. A VirtualBox/VMware image is not
included, and those hypervisors were not separately verified.

## Troubleshooting

| Problem | Action |
| --- | --- |
| `unshare ... Operation not permitted` | Use `sudo` in a VM allowing network namespaces. Some containers forbid them. |
| `/dev/net/tun` missing | Use a standard Linux VM kernel with TUN support. The project does not create device nodes or load modules for you. |
| CMake requires Linux | Run inside Linux, not Windows PowerShell/MinGW. |
| Compiler missing | Install `g++ cmake make` inside the VM. |
| Report file exists | Choose a new `--json` filename; overwriting is intentionally refused. |
| Timeout | Increase `--timeout` up to 120 s, reduce probes, or raise bandwidth. |
| Shaper drops | Inspect categories; increase queue capacity or reduce offered `--send-pps`. |
| Policer does not drop | Offer more traffic or lower the rate; under-budget traffic should pass. |
| Policer exits 0 despite drops | Correct: exit 0 means the experiment completed, not that every packet arrived. |

Exit codes: 0 completed, 1 error/test failure, 2 timeout, 130 interrupted.
Ctrl+C prints metrics and releases owned descriptors. No manual host-route,
firewall, background-service, or named-namespace cleanup is needed.

## GitHub submission

The ZIP has not been uploaded to a GitHub account. Create an empty repository
and upload the extracted contents, preserving the directory structure. Include
source, documentation, and evidence; exclude build directories and local binaries.

Alternatively, from the extracted directory:

```sh
git init
git add .
git commit -m "Add Linux C++ token-bucket traffic-control capstone"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/YOUR_REPOSITORY.git
git push -u origin main
```

Replace the URL with the actual repository. Authenticate when prompted.
An optional Linux GitHub Actions workflow is included; its remote run was not
verified during local development.

Suggested repository title: **Linux C++ Token-Bucket Traffic Control**.
Add your friend's name and batch details to the repository description. Before
**5 October 2026**, verify README rendering, build from a fresh clone in the VM,
run tests, and arrange the trainer evaluation. Your friend should understand
the implementation and describe assistance accurately under the course rules.
